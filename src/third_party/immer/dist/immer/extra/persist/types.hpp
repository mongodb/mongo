#pragma once

#include <immer/extra/persist/detail/alias.hpp>

namespace immer::persist {

struct node_id_tag;
using node_id = detail::type_alias<std::size_t, node_id_tag>;

struct container_id_tag;
using container_id = detail::type_alias<std::size_t, container_id_tag>;

} // namespace immer::persist
