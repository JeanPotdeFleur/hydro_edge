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
};

class RingBuffer {
private:
    std::vector<StereoFrame> buffer;
    size_t head;
    size_t tail;
    size_t max_size;
    size_t current_size;

    std::mutex mtx;
    std::condition_variable cv_consumer;
    std::atomic<bool> active; // Drapeau d'état du pipeline

public:
    RingBuffer(size_t size, size_t payload_bytes_per_cam) 
        : head(0), tail(0), max_size(size), current_size(0), active(true) {
        
        buffer.resize(max_size);
        for (auto& frame : buffer) {
            frame.cam0_data.resize(payload_bytes_per_cam);
            frame.cam1_data.resize(payload_bytes_per_cam);
        }
    }

    void push(const uint8_t* pData0, const uint8_t* pData1, uint64_t ts) {
        std::lock_guard<std::mutex> lock(mtx);

        if (current_size == max_size) {
            throw std::runtime_error("[CRITIQUE] Ring Buffer Overflow !");
        }

        std::memcpy(buffer[head].cam0_data.data(), pData0, buffer[head].cam0_data.size());
        std::memcpy(buffer[head].cam1_data.data(), pData1, buffer[head].cam1_data.size());
        buffer[head].timestamp = ts;

        head = (head + 1) % max_size;
        current_size++;
        cv_consumer.notify_one();
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

        buffer[tail].cam0_data.resize(out_frame.cam0_data.capacity());
        buffer[tail].cam1_data.resize(out_frame.cam1_data.capacity());

        tail = (tail + 1) % max_size;
        current_size--;

        return true;
    }

    // Déclencheur appelé par le Thread 1 lors du Ctrl+C
    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx);
        active = false;
        cv_consumer.notify_all(); // Force le réveil du Thread 2 s'il dormait
    }
};