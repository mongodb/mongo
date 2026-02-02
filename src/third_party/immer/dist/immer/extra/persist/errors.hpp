#pragma once

#include <immer/extra/persist/detail/common/pool.hpp>

#include <stdexcept>

#include <fmt/format.h>

namespace immer::persist {

/**
 * @defgroup persist-exceptions
 */

/**
 * Base class from which all the exceptions in `immer::persist` are derived.
 *
 * @ingroup persist-exceptions
 */
class pool_exception : public std::invalid_argument
{
public:
    using invalid_argument::invalid_argument;
};

/**
 * Thrown when a cycle is detected in the pool of vectors.
 *
 * @ingroup persist-exceptions
 */
class pool_has_cycles : public pool_exception
{
public:
    explicit pool_has_cycles(node_id id)
        : pool_exception{fmt::format("Cycle detected with node ID {}", id)}
    {
    }
};

/**
 * Thrown when a non-existent node is mentioned.
 *
 * @ingroup persist-exceptions
 */
class invalid_node_id : public pool_exception
{
public:
    explicit invalid_node_id(node_id id)
        : pool_exception{fmt::format("Node ID {} is not found", id)}
    {
    }
};

/**
 * Thrown when a non-existent container is mentioned.
 *
 * @ingroup persist-exceptions
 */
class invalid_container_id : public pool_exception
{
public:
    explicit invalid_container_id(container_id id)
        : pool_exception{fmt::format("Container ID {} is not found", id)}
    {
    }
};

/**
 * Thrown when a node has more children than expected.
 *
 * @ingroup persist-exceptions
 */
class invalid_children_count : public pool_exception
{
public:
    explicit invalid_children_count(node_id id)
        : pool_exception{
              fmt::format("Node ID {} has more children than allowed", id)}
    {
    }
};

/**
 * Thrown when duplicate pool name is detected.
 *
 * @ingroup persist-exceptions
 */
class duplicate_name_pool_detected : public pool_exception
{
public:
    explicit duplicate_name_pool_detected(const std::string& pool_name)
        : pool_exception{
              fmt::format("{} pool name has already been used", pool_name)}
    {
    }
};

} // namespace immer::persist
