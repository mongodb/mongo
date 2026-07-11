// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/column/bsoncolumn_expressions_internal.h"
#include "mongo/util/modules.h"


[[MONGO_MOD_PUBLIC]];

namespace mongo::bsoncolumn {

/**
 * Return total number of elements stored in this BSONColumn, count includes missing elements.
 *
 * Throws for invalid BSONColumn binaries, but may not throw for all types of invalid BSON.
 */
size_t count(const char* buffer, size_t size);
size_t count(BSONBinData bin);

/**
 * Returns true if the BSONColumn contains no missing values. For interleaved sections this is
 * computed exactly: a row is a "missing" iff every interleaved sub-stream is missing at that row.
 *
 * Throws for invalid BSONColumn binaries, but may not throw for all types of invalid BSON.
 */
bool dense(const char* buffer, size_t size);
bool dense(BSONBinData bin);

/**
 * Return first non-missing element stored in this BSONColumn
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element first(const char* buffer,
                                      size_t size,
                                      boost::intrusive_ptr<BSONElementStorage> allocator) {
    return internal::first<CMaterializer>(buffer, size, std::move(allocator));
}

template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element first(BSONBinData bin,
                                      boost::intrusive_ptr<BSONElementStorage> allocator) {
    tassert(9095600, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return internal::first<CMaterializer>(reinterpret_cast<const char*>(bin.data),
                                          static_cast<size_t>(bin.length),
                                          std::move(allocator));
}

/**
 * Return last non-missing element stored in this BSONColumn
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element last(const char* buffer,
                                     size_t size,
                                     boost::intrusive_ptr<BSONElementStorage> allocator) {
    return internal::last<CMaterializer>(buffer, size, std::move(allocator));
}
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element last(BSONBinData bin,
                                     boost::intrusive_ptr<BSONElementStorage> allocator) {
    tassert(9095601, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return internal::last<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator));
}

/**
 * Return the 'min' element of a BSONColumn paired with its logical row index (counting missing
 * slots). If the column has no defined values, the element is the materializer's missing
 * representation and the index is undefined.
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
std::pair<typename CMaterializer::Element, size_t> min(
    const char* buffer,
    size_t size,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    return internal::min<CMaterializer>(buffer, size, std::move(allocator), comparator);
}
template <class CMaterializer>
requires Materializer<CMaterializer>
std::pair<typename CMaterializer::Element, size_t> min(
    BSONBinData bin,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    tassert(9095602, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return internal::min<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator), comparator);
}

/**
 * Return the 'max' element of a BSONColumn paired with its logical row index (counting missing
 * slots). If the column has no defined values, the element is the materializer's missing
 * representation and the index is undefined.
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
std::pair<typename CMaterializer::Element, size_t> max(
    const char* buffer,
    size_t size,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    return internal::max<CMaterializer>(buffer, size, std::move(allocator), comparator);
}
template <class CMaterializer>
requires Materializer<CMaterializer>
std::pair<typename CMaterializer::Element, size_t> max(
    BSONBinData bin,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    tassert(9095603, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return internal::max<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator), comparator);
}

/**
 * Return 'min' and 'max' elements in this BSONColumn.
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
typename std::pair<typename CMaterializer::Element, typename CMaterializer::Element> minmax(
    const char* buffer,
    size_t size,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    return internal::minmax<CMaterializer>(buffer, size, std::move(allocator), comparator);
}
template <class CMaterializer>
requires Materializer<CMaterializer>
typename std::pair<typename CMaterializer::Element, typename CMaterializer::Element> minmax(
    BSONBinData bin,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    tassert(9095604, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return internal::minmax<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator), comparator);
}

}  // namespace mongo::bsoncolumn
