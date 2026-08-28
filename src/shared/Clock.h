#pragma once

#include <cstdint>

namespace shared {

// Accumulator-based fixed timestep: advance() feeds real frame time in,
// consumeStep() drains it in fixed-size increments so simulation logic
// runs at a constant rate regardless of frame rate. Drives the cube's
// rotation in src/client/main.cpp; the renderer's hue phase stays
// free-running (see the wiki's Rendering page for why that split exists).
class FixedClock {
public:
    // maxStepsPerAdvance caps the spiral of death: without it, one huge
    // frameDeltaSeconds (a breakpoint, an alt-tab stall) queues an
    // unbounded number of catch-up steps, each one costing real time to
    // simulate and adding more debt than it clears. Clamping the
    // accumulator instead means the sim visibly slows down after a long
    // stall rather than never recovering.
    explicit FixedClock(float fixedDeltaSeconds = 1.0f / 60.0f, int maxStepsPerAdvance = 5)
        : fixedDelta_(fixedDeltaSeconds), maxAccumulator_(fixedDeltaSeconds * static_cast<float>(maxStepsPerAdvance)) {}

    void advance(float frameDeltaSeconds) {
        accumulator_ += frameDeltaSeconds;
        if (accumulator_ > maxAccumulator_) {
            accumulator_ = maxAccumulator_;
        }
    }

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

    // Fraction of a step left in the accumulator: 0 right after a step is
    // consumed, approaching 1 just before the next one fires. main.cpp
    // renders lerp(previousTransform, currentTransform, alpha()) so motion
    // stays smooth at a frame rate that doesn't divide evenly into
    // fixedDelta, at the cost of rendering up to one step behind the
    // latest simulated state (the standard tradeoff of this technique).
    float alpha() const { return accumulator_ / fixedDelta_; }

    uint64_t stepCount() const { return stepCount_; }

private:
    float fixedDelta_;
    float maxAccumulator_;
    float accumulator_ = 0.0f;
    uint64_t stepCount_ = 0;
};

} // namespace shared
