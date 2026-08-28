#pragma once

#include <vector>

union SDL_Event;

namespace client {

enum class Action {
    Pause,
    Quit,
};

// Lifts raw SDL3 key events into named actions, decoupling gameplay/UI
// code from scancodes. Bindings are fixed for this landing; a real rebind
// system is not started.
class ActionMap {
public:
    void processEvent(const SDL_Event& event);

    // Actions triggered since the last call; clears internal state.
    std::vector<Action> consumeActions();

private:
    std::vector<Action> pending_;
};

} // namespace client
