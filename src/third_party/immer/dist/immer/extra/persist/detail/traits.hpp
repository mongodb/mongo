#pragma once

namespace immer::persist::detail {

/**
 * Define these traits to connect a type (vector_one<T>) to its pool
 * (output_pool<T>).
 */
template <class T>
struct container_traits
{};

} // namespace immer::persist::detail
