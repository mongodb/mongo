#ifndef THIRD_PARTY_S2_HASH_H_
#define THIRD_PARTY_S2_HASH_H_

#include "mongo/platform/hash_namespace.h"

#include "mongo/platform/unordered_map.h"
#define hash_map mongo::unordered_map

#include "mongo/platform/unordered_set.h"
#define hash_set mongo::unordered_set

#define HASH_NAMESPACE_START MONGO_HASH_NAMESPACE_START
#define HASH_NAMESPACE_END MONGO_HASH_NAMESPACE_END

// Places that hash-related functions are defined:
// end of s2cellid.h for hashing on S2CellId
// in s2.h and s2.cc for hashing on S2Point
// s2polygon.cc for S2PointPair

#endif
