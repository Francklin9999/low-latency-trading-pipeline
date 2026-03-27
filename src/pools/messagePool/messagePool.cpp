#include <numeric>
#include <cstdlib>
#include <stdexcept>
#include "hft/pools/messagePool/messagePool.hpp"


MessagePool::MessagePool(const int32_t nMessages) :  
  base_(nullptr)
, capacity_(nMessages)
, slots_(nMessages)
{ 
    base_ = static_cast<uint8_t*>(malloc(MESSAGE_POOL_PACKET_SIZE * nMessages));
    if (!base_)
        throw std::runtime_error("Failed to allocate message pool");
    std::iota(slots_.begin(), slots_.end(), 0);
}

MessagePool::~MessagePool()
{
    free(base_);
}

uint8_t* MessagePool::getMessage()
{
    if (slots_.empty()) {
        throw std::runtime_error("No more memory");
    }
    uint8_t* ptr = base_ + (slots_.back() * MESSAGE_POOL_PACKET_SIZE);  
    slots_.pop_back();
    return ptr;
}

void MessagePool::removeMessage(const uint8_t* ptr) noexcept
{   
    if (ptr == nullptr || ptr < base_ || ptr >= base_ + (capacity_ * MESSAGE_POOL_PACKET_SIZE))
        return;

    slots_.push_back(static_cast<int32_t>((ptr - base_) / MESSAGE_POOL_PACKET_SIZE));
} 