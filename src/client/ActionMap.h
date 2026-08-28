#pragma once

#include <vector>

union SDL_Event;

namespace client {

enum class Action {
    Pause,
    Quit,
};

// Pure data, no meaning attached: mouseDeltaX/Y accumulate raw SDL mouse
// motion since the last consumeAxes() call, then reset. moveX/moveY are
// WASD read as a -1..1 pair (x: D-A, y: W-S), sampled live rather than
// accumulated. Nothing here is a camera basis, a speed, or scaled by
// frame time - a fly/orbit/FPS controller that turns these into motion
// belongs in a layer-2 fork, not here.
struct Axes {
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float moveX = 0.0f;
    float moveY = 0.0f;
};

// Lifts raw SDL3 key/mouse events into named actions and axes, decoupling
// gameplay/UI code from scancodes. Bindings are fixed; no rebind system
// exists.
class ActionMap {
public:
    void processEvent(const SDL_Event& event);

    // Actions triggered since the last call; clears internal state.
    std::vector<Action> consumeActions();

    // Mouse delta since the last call (then reset); WASD sampled live.
    Axes consumeAxes();

private:
    std::vector<Action> pending_;
    float mouseDeltaX_ = 0.0f;
    float mouseDeltaY_ = 0.0f;
    bool moveForward_ = false;
    bool moveBack_ = false;
    bool moveLeft_ = false;
    bool moveRight_ = false;
};

} // namespace client
