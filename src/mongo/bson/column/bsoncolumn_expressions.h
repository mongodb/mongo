/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/column/bsoncolumn_expressions_internal.h"

namespace mongo::bsoncolumn {

/**
 * Return first non-missing element stored in this BSONColumn
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element first(const char* buffer,
                                      size_t size,
                                      boost::intrusive_ptr<BSONElementStorage> allocator) {
    return bsoncolumn_internal::first<CMaterializer>(buffer, size, std::move(allocator));
}

template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element first(BSONBinData bin,
                                      boost::intrusive_ptr<BSONElementStorage> allocator) {
    tassert(9095600, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return bsoncolumn_internal::first<CMaterializer>(reinterpret_cast<const char*>(bin.data),
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
    return bsoncolumn_internal::last<CMaterializer>(buffer, size, std::move(allocator));
}
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element last(BSONBinData bin,
                                     boost::intrusive_ptr<BSONElementStorage> allocator) {
    tassert(9095601, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return bsoncolumn_internal::last<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator));
}

/**
 * Return 'min' element in this BSONColumn.
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element min(const char* buffer,
                                    size_t size,
                                    boost::intrusive_ptr<BSONElementStorage> allocator,
                                    const StringDataComparator* comparator = nullptr) {
    return bsoncolumn_internal::min<CMaterializer>(buffer, size, std::move(allocator), comparator);
}
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element min(BSONBinData bin,
                                    boost::intrusive_ptr<BSONElementStorage> allocator,
                                    const StringDataComparator* comparator = nullptr) {
    tassert(9095602, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return bsoncolumn_internal::min<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator), comparator);
}

/**
 * Return 'max' element in this BSONColumn.
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element max(const char* buffer,
                                    size_t size,
                                    boost::intrusive_ptr<BSONElementStorage> allocator,
                                    const StringDataComparator* comparator = nullptr) {
    return bsoncolumn_internal::max<CMaterializer>(buffer, size, std::move(allocator), comparator);
}
template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element max(BSONBinData bin,
                                    boost::intrusive_ptr<BSONElementStorage> allocator,
                                    const StringDataComparator* comparator = nullptr) {
    tassert(9095603, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return bsoncolumn_internal::max<CMaterializer>(
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
    return bsoncolumn_internal::minmax<CMaterializer>(
        buffer, size, std::move(allocator), comparator);
}
template <class CMaterializer>
requires Materializer<CMaterializer>
typename std::pair<typename CMaterializer::Element, typename CMaterializer::Element> minmax(
    BSONBinData bin,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator = nullptr) {
    tassert(9095604, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return bsoncolumn_internal::minmax<CMaterializer>(
        reinterpret_cast<const char*>(bin.data), bin.length, std::move(allocator), comparator);
}

}  // namespace mongo::bsoncolumn
