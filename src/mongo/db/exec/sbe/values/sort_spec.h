/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo::sbe::value {
/**
 * SortSpec is a wrapper around a BSONObj giving a sort pattern (encoded as a BSONObj), a collator,
 * and a SortKeyGenerator object.
 */
class SortSpec {
public:
    SortSpec(const BSONObj& sortPatternBson)
        : _sortPatternBson(sortPatternBson.getOwned()),
          _sortPattern(_sortPatternBson, nullptr /* expCtx, needed for meta sorts */),
          _sortKeyGen(_sortPattern, nullptr /* collator */) {
        _localBsonEltStorage.resize(_sortPattern.size());
        _localSortKeyComponentStorage.elts.resize(_sortPattern.size());
    }
    SortSpec(const SortSpec& other)
        : _sortPatternBson(other._sortPatternBson),
          _sortPattern(other._sortPattern),
          _sortKeyGen(_sortPattern, nullptr /* collator */) {
        _localBsonEltStorage.resize(_sortPattern.size());
        _localSortKeyComponentStorage.elts.resize(_sortPattern.size());
    }

    SortSpec& operator=(const SortSpec&) = delete;

    KeyString::Value generateSortKey(const BSONObj& obj, const CollatorInterface* collator);


    /*
     * Creates a sort key that's cheaper to generate but more expensive to compare.
     *
     * The underlying memory for the returned SortKeyComponentVector is owned by the SortSpec
     * itself. It is the caller's responsibility to ensure the SortSpec remains alive while the
     * return value of this function is used. The return value is valid until the next call to
     * generateSortKeyComponentVector().
     *
     * If the passed in 'obj' is owned, this class takes ownership of it. If it is not owned,
     * the passed in obj must remain alive as long as the return value from this function may
     * be used.
     */
    value::SortKeyComponentVector* generateSortKeyComponentVector(
        FastTuple<bool, value::TypeTags, value::Value> obj, const CollatorInterface* collator);

    /**
     * Compare an array of values based on the sort pattern.
     */
    std::pair<TypeTags, Value> compare(TypeTags leftTag,
                                       Value leftVal,
                                       TypeTags rightTag,
                                       Value rightVal,
                                       const CollatorInterface* collator = nullptr) const;

    const BSONObj& getPattern() const {
        return _sortPatternBson;
    }

    size_t getApproximateSize() const;

private:
    BtreeKeyGenerator initKeyGen() const;

    const BSONObj _sortPatternBson;

    const SortPattern _sortPattern;
    SortKeyGenerator _sortKeyGen;

    // Storage for the sort key component vector returned by generateSortKeyComponentVector().
    std::vector<BSONElement> _localBsonEltStorage;
    value::SortKeyComponentVector _localSortKeyComponentStorage;

    // These members store objects that may be held by the key generator. For example, if the
    // caller generates keys using an object that is temporary, it will get stashed here so that it
    // remains alive while the sort keys can be used.
    BSONObj _tempObj;
    boost::optional<ValueGuard> _tempVal;
};
}  // namespace mongo::sbe::value
