#pragma once

namespace immer::persist {

/**
 * The wrapper is used to enable the incompatible hash mode which is required
 * when the key of a hash-based container transformed in a way that changes its
 * hash.
 *
 * A value of this type should be returned from a transforming function
 * accepting `target_container_type_request`.
 *
 * @ingroup persist-transform
 * @see
 * @rst
 * :ref:`modifying-the-hash-of-the-id`
 * @endrst
 */
template <class Container>
struct incompatible_hash_wrapper
{};

/**
 * This type is used as an argument for a transforming function.
 * The return type of the function is used to specify the desired container type
 * to contain the transformed values.
 *
 * @ingroup persist-transform
 * @see
 * @rst
 * :ref:`transforming-hash-based-containers`
 * @endrst
 */
struct target_container_type_request
{};

} // namespace immer::persist
