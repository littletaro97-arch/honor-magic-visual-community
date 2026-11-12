#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace magic::feedback {
// WMI reports a changed value, not who changed it. Use session input only as a
// bounded eligibility signal for external changes. Explicit successful slider
// actions do not depend on this heuristic. No input hook or polling is needed.
inline constexpr std::uint32_t BrightnessFeedbackWindowMs = 2000;
inline bool RecentBrightnessInput(std::uint32_t now, std::uint32_t last, bool known) {
    return known && static_cast<std::uint32_t>(now-last) <= BrightnessFeedbackWindowMs;
}
struct BrightnessFeedbackCursor {
    std::wstring lastKey;
    bool Consume(std::wstring const &key, double timestamp, std::uint64_t now, bool allowed) {
        if(key.empty() || key==lastKey)return false;
        lastKey=key; // Even muted/stale events are consumed; never replay later.
        return allowed && std::isfinite(timestamp) && timestamp>0 && timestamp<=now &&
            static_cast<double>(now)-timestamp<=BrightnessFeedbackWindowMs;
    }
};
}
