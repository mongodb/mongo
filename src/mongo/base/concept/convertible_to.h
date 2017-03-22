#pragma once

namespace mongo {
namespace concept {
/**
 * The ConvertibleTo concept models a type which can be converted implicitly into a `T`.
 * The code: `T x; x= ConvertibleTo< T >{};` should be valid.
 */
template <typename T>
struct ConvertibleTo {
    operator T();
}
}  // namespace concept
}  // namespace mongo
