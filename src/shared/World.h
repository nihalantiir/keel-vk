#pragma once

#include <entt/entt.hpp>

#include <utility>

namespace keel {

using EntityId = entt::entity;

// Thin facade over entt::registry so callers depend on keel::World instead
// of EnTT directly. Holds engine-level components only (Transform, Name,
// NetId, Bounds, tags like Visible, see Components.h); gameplay components
// belong to whatever system builds on this, not here.
class World {
public:
    EntityId createEntity() { return registry_.create(); }
    void destroyEntity(EntityId entity) { registry_.destroy(entity); }

    // decltype(auto), not Component&: EnTT's emplace() returns void for
    // empty (tag) component types, since there's no data to reference.
    template <typename Component, typename... Args>
    decltype(auto) addComponent(EntityId entity, Args&&... args) {
        return registry_.emplace<Component>(entity, std::forward<Args>(args)...);
    }

    template <typename Component>
    Component& getComponent(EntityId entity) {
        return registry_.get<Component>(entity);
    }

    template <typename Component>
    Component* tryGetComponent(EntityId entity) {
        return registry_.try_get<Component>(entity);
    }

    template <typename... Components, typename Func>
    void each(Func&& func) {
        registry_.view<Components...>().each(std::forward<Func>(func));
    }

private:
    entt::registry registry_;
};

} // namespace keel
