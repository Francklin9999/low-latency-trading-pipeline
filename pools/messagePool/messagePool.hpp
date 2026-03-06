#pragma once

#include <cstdint>
#include <vector>

#define MESSAGE_POOL_PACKET_SIZE 1472

class MessagePool
{
public:
    explicit MessagePool(const int32_t nMessages);

    ~MessagePool();

    uint8_t* getMessage();
    void removeMessage(const uint8_t*) noexcept;
    uint8_t* getBase() const noexcept { return base_; }

private:
    uint8_t* base_;
    uint32_t capacity_;
    std::vector<int32_t> slots_;
};