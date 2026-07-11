// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {
/**
 * SortSpec is a wrapper around a BSONObj giving a sort pattern (encoded as a BSONObj), a collator,
 * and a SortKeyGenerator object.
 */
class SortSpec {
public:
    using TypeTags = value::TypeTags;
    using Value = value::Value;
    explicit SortSpec(
        const BSONObj& sortPatternBson,
        const boost::intrusive_ptr<ExpressionContext>& expCtx = nullptr /* needed for meta sorts */)
        : _sortPatternBson(sortPatternBson.getOwned()),
          _sortPattern(_sortPatternBson, expCtx),
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

    key_string::Value generateSortKey(const BSONObj& obj, const CollatorInterface* collator);


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
        value::TagValueMaybeOwned obj, const CollatorInterface* collator);

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

    const SortPattern& getSortPattern() const {
        return _sortPattern;
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
    value::TagValueMaybeOwned _tempVal;
};
}  // namespace mongo::sbe
