#pragma once

#include <cstdint>
#include <vector>
#include "events.h"

class EventPool
{
public:
    explicit EventPool(const int32_t nEvents);

    ~EventPool();

    int32_t availableCount() const noexcept;

    event* getEvent();
    void removeEvent(const event*) noexcept;

private:
    event* base_;
    uint32_t capacity_;
    std::vector<int32_t> slots_;
};
