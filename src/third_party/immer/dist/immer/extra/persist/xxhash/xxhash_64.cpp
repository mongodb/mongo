#include "xxhash.hpp"

#include <xxhash.h>

namespace immer::persist {

std::uint64_t xx_hash_value_string(const std::string& str)
{
    return XXH3_64bits(str.c_str(), str.size());
}

} // namespace immer::persist
