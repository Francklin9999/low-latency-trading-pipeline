#pragma once

#include "hft/engine/dispatcher.hpp"

namespace cpu_op {

void run_lane(BatchBuffer& b, int lane);

}