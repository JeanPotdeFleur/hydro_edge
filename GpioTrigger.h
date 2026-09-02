// GpioTrigger.h
//
// Hardware trigger pulse generation for the Hopkins rocky-shore station.
//
// Under software triggering the two sensors are commanded by two successive
// TriggerSoftware calls, which are two sequential USB transactions: the
// cameras do not expose at the same instant. This replaces them with a single
// register write that drops both GPIO lines together, so both sensors see the
// same falling edge and expose together.
//
// --- Why a single bulk write and not two calls ---
//
// libgpiod's set_value_bulk over lines belonging to one chip resolves to one
// ioctl and one RP1 register access. Two separate calls would reintroduce
// several microseconds of jitter between the two edges, which is the whole
// quantity this exists to remove. The two lines are therefore requested as a
// bulk at burst start and never addressed individually.
//
// --- Why open drain ---
//
// Line2 of the Blackfly S reads 1 with nothing connected: it is held high by
// an internal pull-up. The Pi therefore only ever pulls low and releases; the
// high level is supplied by the camera. No contention is possible even if the
// line were misconfigured, and with the Pi unpowered the line simply stays
// high rather than floating, so no spurious trigger is produced.
//
// The same pin carries a firmware-selectable 3.3 V rail capable of 120 mA
// under V3_3Enable, which the acquisition binary forces off before configuring
// the line as an input. That guard is what makes it safe to drive this net at
// all, and it belongs to the camera configuration rather than here.
//
// --- Why the request is held for the whole burst ---
//
// Opening the chip and requesting lines costs a syscall and an allocation.
// Doing it four times a second, on the one code path whose punctuality is the
// measurement, would be paying jitter for nothing.

#pragma once

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include <gpiod.h>

// libgpiod 1.6 on this station: the v2 C API is incompatible and an example
// found online will almost certainly assume it. GPIOD_LINE_BULK_MAX_LINES and
// the whole bulk API were removed in v2, so its absence is the discriminant.
#if !defined(GPIOD_LINE_BULK_MAX_LINES)
#error "gpiod.h without the bulk API: this file targets libgpiod 1.x"
#endif

class GpioTrigger
{
public:
    // chip_name is the gpiod chip label, not a path. On the Raspberry Pi 5 the
    // 40-pin header is exposed by pinctrl-rp1 as gpiochip4, where every
    // previous generation used gpiochip0; an example written for a Pi 4 will
    // silently address an internal Broadcom controller with no header pins.
    GpioTrigger(const std::string& chip_name,
                unsigned int       offset_a,
                unsigned int       offset_b,
                unsigned int       pulse_us,
                const std::string& consumer = "hydro_edge")
        : pulse_us_(pulse_us)
    {
        chip_ = gpiod_chip_open_by_name(chip_name.c_str());
        if (!chip_)
        {
            throw std::runtime_error("cannot open " + chip_name + ": " +
                                     std::strerror(errno) +
                                     ". On a Raspberry Pi 5 the header is gpiochip4.");
        }

        const unsigned int offsets[2] = {offset_a, offset_b};
        if (gpiod_chip_get_lines(chip_, const_cast<unsigned int*>(offsets), 2, &bulk_) < 0)
        {
            const std::string e = std::strerror(errno);
            gpiod_chip_close(chip_);
            chip_ = nullptr;
            throw std::runtime_error("cannot get lines " + std::to_string(offset_a) + " and " +
                                     std::to_string(offset_b) + ": " + e);
        }

        // Released, so the camera pull-ups hold both lines high before the
        // first pulse and no edge is produced by the request itself.
        const int initial[2] = {1, 1};
        if (gpiod_line_request_bulk_output_flags(
                &bulk_, consumer.c_str(), GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN, initial) < 0)
        {
            const std::string e = std::strerror(errno);
            gpiod_chip_close(chip_);
            chip_ = nullptr;
            throw std::runtime_error(
                "cannot request lines as open-drain outputs: " + e +
                ". Check that no other process holds them: gpioinfo " + chip_name);
        }
        held_ = true;
    }

    ~GpioTrigger()
    {
        if (held_)
        {
            const int high[2] = {1, 1};
            gpiod_line_set_value_bulk(&bulk_, high);
            gpiod_line_release_bulk(&bulk_);
        }
        if (chip_) gpiod_chip_close(chip_);
    }

    GpioTrigger(const GpioTrigger&)            = delete;
    GpioTrigger& operator=(const GpioTrigger&) = delete;

    // One falling edge on both lines at the same instant, held for pulse_us,
    // then released. Returns false rather than throwing: this is called from
    // the producer thread, where an exception crossing the thread boundary is
    // an abort with extra steps, and a failed pulse is a countable outcome
    // like any other.
    bool pulse()
    {
        const int low[2]  = {0, 0};
        const int high[2] = {1, 1};

        if (gpiod_line_set_value_bulk(&bulk_, low) < 0) return false;

        // clock_nanosleep on the monotonic clock rather than usleep: the
        // duration here is a width, not an instant, and it must not be
        // perturbed by a step of the realtime clock.
        struct timespec w{};
        w.tv_sec  = 0;
        w.tv_nsec = static_cast<long>(pulse_us_) * 1000L;
        while (clock_nanosleep(CLOCK_MONOTONIC, 0, &w, &w) == EINTR) {}

        return gpiod_line_set_value_bulk(&bulk_, high) >= 0;
    }

private:
    gpiod_chip*      chip_ = nullptr;
    gpiod_line_bulk  bulk_{};
    bool             held_     = false;
    unsigned int     pulse_us_ = 100;
};