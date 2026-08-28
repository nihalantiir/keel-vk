#include "ActionMap.h"

#include <SDL3/SDL.h>

namespace client {

void ActionMap::processEvent(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }
    switch (event.key.scancode) {
        case SDL_SCANCODE_SPACE:
            pending_.push_back(Action::Pause);
            break;
        case SDL_SCANCODE_ESCAPE:
            pending_.push_back(Action::Quit);
            break;
        default:
            break;
    }
}

std::vector<Action> ActionMap::consumeActions() {
    std::vector<Action> actions = std::move(pending_);
    pending_.clear();
    return actions;
}

} // namespace client
