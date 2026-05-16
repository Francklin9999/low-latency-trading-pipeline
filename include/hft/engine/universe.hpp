#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace hft::universe {

// Top-15 NASDAQ tickers by post-09:30 ITCH book-event count on the bundled file;
// locates are file-specific (re-run scripts/utils/top_symbols.py if you swap files).
inline constexpr auto kLocates = std::to_array<std::uint16_t>({
    6562,   // QQQ
    7457,   // SPY
    7939,   // TQQQ
    347,    // AMD
    4321,   // IWM
    5294,   // MSFT
    2741,   // FB
    13,     // AAPL
    7159,   // SH
    2020,   // DIA
    8000,   // TSLA
    7888,   // TNA
    8036,   // TVIX
    8757,   // XLK
    5336,   // MU
});

inline constexpr auto kNames = std::to_array<const char*>({
    "QQQ", "SPY", "TQQQ", "AMD",  "IWM",
    "MSFT","FB",  "AAPL", "SH",   "DIA",
    "TSLA","TNA", "TVIX", "XLK",  "MU",
});
// Keep names in lock-step with the locates above.

inline constexpr std::size_t kSize = kLocates.size();
static_assert(kSize == kNames.size(),
              "kNames must stay in sync with kLocates");

inline constexpr std::uint16_t kNotInUniverse =
    std::numeric_limits<std::uint16_t>::max();

namespace detail {
inline constexpr auto build_shard_lut()
{
    std::array<std::uint16_t, 65536> lut{};
    for (auto& v : lut) v = kNotInUniverse;
    for (std::size_t i = 0; i < kSize; ++i) {
        lut[kLocates[i]] = static_cast<std::uint16_t>(i);
    }
    return lut;
}
inline constexpr auto kShardLut = build_shard_lut();
}  // namespace detail

inline constexpr std::uint16_t shard_for_locate(std::uint16_t locate) noexcept
{
    return detail::kShardLut[locate];
}

inline constexpr bool in_universe(std::uint16_t locate) noexcept
{
    return shard_for_locate(locate) != kNotInUniverse;
}

}  // namespace hft::universe
