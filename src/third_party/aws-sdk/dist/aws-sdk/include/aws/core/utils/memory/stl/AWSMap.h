/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSAllocator.h>

#include <map>
#include <unordered_map>
#include <cstring>
#include <functional>

namespace Aws
{

template< typename K, typename V > using Map = std::map< K, V, std::less< K >, Aws::Allocator< std::pair< const K, V > > >;
template< typename K, typename V > using UnorderedMap = std::unordered_map< K, V, std::hash< K >, std::equal_to< K >, Aws::Allocator< std::pair< const K, V > > >;
template< typename K, typename V > using MultiMap = std::multimap< K, V, std::less< K >, Aws::Allocator< std::pair< const K, V > > >;

struct CompareStrings
{
    bool operator()(const char* a, const char* b) const
    {
        return std::strcmp(a, b) < 0;
    }
};

template<typename V> using CStringMap = std::map<const char*, V, CompareStrings, Aws::Allocator<std::pair<const char*, V> > >;

template<typename Key, typename SourceValue, typename DestinationValue>
void TransformAndInsert(const Aws::UnorderedMap<Key,SourceValue> &src,
    Aws::Map<Key, DestinationValue> &dst,
    std::function<DestinationValue (const SourceValue&)> mappingFunc)
{
    for (const auto& srcPair: src)
    {
        dst.emplace(srcPair.first, mappingFunc(srcPair.second));
    }
}

template<typename K, typename V>
V GetWithDefault(const Aws::Map<K,V> &map, const K &key, V &&defaultValue) {
    typename Aws::Map<K,V>::const_iterator it = map.find(key);
    if ( it == map.end() ) {
        return std::forward<V>(defaultValue);
    }
    return it->second;
}

} // namespace Aws
