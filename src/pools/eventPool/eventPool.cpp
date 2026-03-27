#include <numeric>
#include <cstdlib>
#include <stdexcept>
#include "hft/pools/eventPool/eventPool.hpp"

EventPool::EventPool(const int32_t nEvents) :
  base_(nullptr)
, capacity_(nEvents)
, slots_(nEvents)
{
    base_ = static_cast<event*>(malloc(sizeof(event) * nEvents));
    std::iota(slots_.begin(), slots_.end(), 0);
}

EventPool::~EventPool()
{
    free(base_);
}

int32_t EventPool::availableCount() const noexcept
{
    return static_cast<int32_t>(slots_.size());
}

event* EventPool::getEvent()
{
    if (slots_.empty()) {
        throw std::runtime_error("No more event slots");
    }
    event* ptr = base_ + slots_.back();
    slots_.pop_back();
    return ptr;
}

void EventPool::removeEvent(const event* ptr) noexcept
{
    if (ptr == nullptr || ptr < base_ || ptr >= base_ + capacity_)
        return;

    slots_.push_back(static_cast<int32_t>(ptr - base_));
}
