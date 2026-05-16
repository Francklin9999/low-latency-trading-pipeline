#pragma once

namespace hft::strategy {
class MarketMaker;
}

namespace hft::telemetry {

void start(strategy::MarketMaker* mm) noexcept;
void stop() noexcept;

}  // namespace hft::telemetry
