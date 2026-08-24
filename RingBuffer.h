#pragma once

#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <stdexcept>
#include <atomic>

struct StereoFrame {
    std::vector<uint8_t> cam0_data;
    std::vector<uint8_t> cam1_data;
    uint64_t timestamp;
    // Trigger ordinal within the burst, starting at 1. Carried through the
    // buffer so that the consumer names files by the instant of exposure
    // rather than by its own write count: a frame lost upstream must leave a
    // gap, never shift every subsequent index and silently de-align the two
    // cameras.
    uint64_t index;
};

class RingBuffer {
private:
    std::vector<StereoFrame> buffer;
    size_t head;
    size_t tail;
    size_t max_size;
    size_t current_size;
    // Deepest occupancy observed over the burst. The single most informative
    // number about how close the pipeline came to losing a frame: a high
    // water mark far below capacity means the drive kept up, one approaching
    // capacity means the margin is thinner than the nominal rate suggests.
    size_t high_water;
    // Rejected pushes over the burst. Distinct from the high water mark: the
    // mark says how close the pipeline came, this says how often it failed.
    size_t overflow_count;

    std::mutex mtx;
    std::condition_variable cv_consumer;
    std::atomic<bool> active; // Drapeau d'état du pipeline

public:
    RingBuffer(size_t size, size_t payload_bytes_per_cam) 
        : head(0), tail(0), max_size(size), current_size(0), high_water(0), overflow_count(0), active(true) {
        
        buffer.resize(max_size);
        for (auto& frame : buffer) {
            frame.cam0_data.resize(payload_bytes_per_cam);
            frame.cam1_data.resize(payload_bytes_per_cam);
        }
    }

    // Returns false when the buffer is full, in which case nothing is stored
    // and the caller decides what to do about it.
    //
    // The earlier implementation threw a std::runtime_error here. That
    // exception escaped the producer thread, where no handler matched it, and
    // terminated the process: the very RAM the buffer exists to protect was
    // discarded at the exact moment it mattered. Overflow is a foreseeable
    // consequence of a slow drive, not an exceptional condition, and an
    // exception crossing a thread boundary is never a signalling mechanism.
    bool push(const uint8_t* pData0, const uint8_t* pData1, uint64_t ts, uint64_t index) {
        std::lock_guard<std::mutex> lock(mtx);

        if (current_size == max_size) {
            ++overflow_count;
            return false;
        }

        std::memcpy(buffer[head].cam0_data.data(), pData0, buffer[head].cam0_data.size());
        std::memcpy(buffer[head].cam1_data.data(), pData1, buffer[head].cam1_data.size());
        buffer[head].timestamp = ts;
        buffer[head].index = index;

        head = (head + 1) % max_size;
        current_size++;
        if (current_size > high_water) high_water = current_size;
        cv_consumer.notify_one();
        return true;
    }

    bool pop(StereoFrame& out_frame) {
        std::unique_lock<std::mutex> lock(mtx);
        
        // Attente conditionnelle : le buffer contient des données OU a reçu l'ordre d'extinction
        cv_consumer.wait(lock, [this]() { return current_size > 0 || !active; });

        // Extinction propre : le buffer est vide et désactivé
        if (current_size == 0 && !active) {
            return false; 
        }

        out_frame.cam0_data.swap(buffer[tail].cam0_data);
        out_frame.cam1_data.swap(buffer[tail].cam1_data);
        out_frame.timestamp = buffer[tail].timestamp;
        out_frame.index = buffer[tail].index;

        buffer[tail].cam0_data.resize(out_frame.cam0_data.capacity());
        buffer[tail].cam1_data.resize(out_frame.cam1_data.capacity());

        tail = (tail + 1) % max_size;
        current_size--;

        return true;
    }

    size_t occupancy() {
        std::lock_guard<std::mutex> lock(mtx);
        return current_size;
    }

    size_t highWater() {
        std::lock_guard<std::mutex> lock(mtx);
        return high_water;
    }

    size_t overflows() {
        std::lock_guard<std::mutex> lock(mtx);
        return overflow_count;
    }

    size_t capacity() const { return max_size; }

    // Déclencheur appelé par le Thread 1 lors du Ctrl+C
    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx);
        active = false;
        cv_consumer.notify_all(); // Force le réveil du Thread 2 s'il dormait
    }
};