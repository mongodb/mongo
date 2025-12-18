// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{

template <class T>
inline void GetHash(size_t &seed, const T &arg)
{
  std::hash<T> hasher;
  // reference -
  // https://www.boost.org/doc/libs/1_37_0/doc/html/hash/reference.html#boost.hash_combine
  seed ^= hasher(arg) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <class T>
inline void GetHash(size_t &seed, const std::vector<T> &arg)
{
  for (auto v : arg)
  {
    GetHash<T>(seed, v);
  }
}

// Specialization for const char*
// this creates an intermediate string.
template <>
inline void GetHash<const char *>(size_t &seed, const char *const &arg)
{
  std::hash<std::string> hasher;
  seed ^= hasher(std::string(arg)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct GetHashForAttributeValueVisitor
{
  GetHashForAttributeValueVisitor(size_t &seed) : seed_(seed) {}
  template <class T>
  void operator()(T &v)
  {
    GetHash(seed_, v);
  }
  size_t &seed_;
};

// Calculate hash of keys and values of attribute map
inline size_t GetHashForAttributeMap(const OrderedAttributeMap &attribute_map)
{
  size_t seed = 0UL;
  for (auto &kv : attribute_map)
  {
    GetHash(seed, kv.first);
    nostd::visit(GetHashForAttributeValueVisitor(seed), kv.second);
  }
  return seed;
}

template <class T>
inline size_t GetHash(T value)
{
  std::hash<T> hasher;
  return hasher(value);
}

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
