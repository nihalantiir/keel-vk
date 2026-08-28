#pragma once

#include <cstdint>

namespace shared {

// Accumulator-based fixed timestep: advance() feeds real frame time in,
// consumeStep() drains it in fixed-size increments so simulation logic
// runs at a constant rate regardless of frame rate. Not used by the
// renderer's own animation (that stays free-running for smooth motion);
// this is the utility future fixed-rate systems (physics, networked
// simulation) are expected to build on.
class FixedClock {
public:
    explicit FixedClock(float fixedDeltaSeconds = 1.0f / 60.0f) : fixedDelta_(fixedDeltaSeconds) {}

    void advance(float frameDeltaSeconds) { accumulator_ += frameDeltaSeconds; }

    // Call in a loop until it returns false to drain every pending step.
    bool consumeStep() {
        if (accumulator_ < fixedDelta_) {
            return false;
        }
        accumulator_ -= fixedDelta_;
        ++stepCount_;
        return true;
    }

    float fixedDelta() const { return fixedDelta_; }

    // Fraction of a step left in the accumulator, for interpolating render
    // state between the last two simulation steps. Unused until a system
    // actually needs it.
    float alpha() const { return accumulator_ / fixedDelta_; }

    uint64_t stepCount() const { return stepCount_; }

private:
    float fixedDelta_;
    float accumulator_ = 0.0f;
    uint64_t stepCount_ = 0;
};

} // namespace shared
