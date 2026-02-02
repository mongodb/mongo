#pragma once

#include <string>

namespace immer::persist {

/**
 * xxHash is a good option to be used with `immer::persist` as it produces
 * hashes identical across all platforms.
 *
 * @see https://xxhash.com/
 * @ingroup persist-api
 */
template <class T>
struct xx_hash
{
    std::uint64_t operator()(const T& val) const { return xx_hash_value(val); }
};

std::uint64_t xx_hash_value_string(const std::string& str);

template <>
struct xx_hash<std::string>
{
    std::uint64_t operator()(const std::string& val) const
    {
        return xx_hash_value_string(val);
    }
};

} // namespace immer::persist
