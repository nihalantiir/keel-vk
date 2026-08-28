#include "ActionMap.h"

#include <SDL3/SDL.h>

namespace client {

void ActionMap::processEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        mouseDeltaX_ += event.motion.xrel;
        mouseDeltaY_ += event.motion.yrel;
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        switch (event.key.scancode) {
            case SDL_SCANCODE_W:
                moveForward_ = down;
                break;
            case SDL_SCANCODE_S:
                moveBack_ = down;
                break;
            case SDL_SCANCODE_A:
                moveLeft_ = down;
                break;
            case SDL_SCANCODE_D:
                moveRight_ = down;
                break;
            default:
                break;
        }
    }

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

Axes ActionMap::consumeAxes() {
    Axes axes;
    axes.mouseDeltaX = mouseDeltaX_;
    axes.mouseDeltaY = mouseDeltaY_;
    mouseDeltaX_ = 0.0f;
    mouseDeltaY_ = 0.0f;
    axes.moveX = (moveRight_ ? 1.0f : 0.0f) - (moveLeft_ ? 1.0f : 0.0f);
    axes.moveY = (moveForward_ ? 1.0f : 0.0f) - (moveBack_ ? 1.0f : 0.0f);
    return axes;
}

} // namespace client
