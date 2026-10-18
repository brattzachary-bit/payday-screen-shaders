#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

// A latest-packet mailbox, like the prior rewritten file. The payload itself
// carries the four-pose history. Every reader receives its own mutable copy.
template<size_t Capacity> class BoundedMailbox {
    std::mutex mutex;
    char bytes[Capacity]{};
    size_t size = 0;
    uint64_t generation = 0;
public:
    bool Publish(const char* text, size_t count) {
        if (!text || !count || count >= Capacity || std::memchr(text, 0, count)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        std::memcpy(bytes, text, count); bytes[count] = 0;
        size = count; ++generation;
        return true;
    }
    bool Take(uint64_t& consumed, char (&output)[Capacity], size_t& count) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!generation || generation == consumed) return false;
        std::memcpy(output, bytes, size + 1); count = size; consumed = generation;
        return true;
    }
};
