#pragma once

#include <immer/extra/persist/detail/cereal/pools.hpp>

namespace immer::persist::detail {

template <class Storage, class Container>
auto get_container_id(const output_pools<Storage>& pools,
                      const Container& container)
{
    const auto& old_pool =
        pools.template get_output_pool<std::decay_t<Container>>();
    const auto [new_pool, id] = add_to_pool(container, old_pool);
    if (!(new_pool == old_pool)) {
        throw std::logic_error{
            "Expecting that the container has already been persisted"};
    }
    return id;
}

} // namespace immer::persist::detail
