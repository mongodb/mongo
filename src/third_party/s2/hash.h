#ifndef THIRD_PARTY_S2_HASH_H_
#define THIRD_PARTY_S2_HASH_H_

#include <unordered_map>
#define hash_map std::unordered_map

#include <unordered_set>
#define hash_set std::unordered_set

#define HASH_NAMESPACE_START namespace std {
#define HASH_NAMESPACE_END }

// Places that hash-related functions are defined:
// end of s2cellid.h for hashing on S2CellId
// in s2.h and s2.cc for hashing on S2Point
// s2polygon.cc for S2PointPair

#endif
