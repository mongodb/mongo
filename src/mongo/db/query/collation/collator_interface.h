/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/query/collation/collation_spec.h"

namespace mongo {

/**
 * An interface for ordering and matching according to a collation. Instances should be retrieved
 * from the CollatorFactoryInterface and may not be copied.
 *
 * All methods are thread-safe.
 *
 * Does not throw exceptions.
 */
class CollatorInterface : public StringData::ComparatorInterface {
    CollatorInterface(const CollatorInterface&) = delete;
    CollatorInterface& operator=(const CollatorInterface&) = delete;

public:
    /**
     * Every string has a corresponding ComparisonKey with respect to this collator. Two
     * ComparisonKeys can be lexicographically ordered in order to obtain the collation's sort order
     * and equivalence classes.
     *
     * A ComparisonKey is logically an owned array of bytes. It is cheap to move but potentially
     * expensive to copy.
     *
     * ComparisonKeys may only be obtained via CollatorInterface::getComparisonKey().
     *
     * In general, two strings should be compared with respect to a collation using
     * CollatorInterface::compare(). ComparisonKey::compare() may be faster if repeatedly comparing
     * the same string(s).
     */
    class ComparisonKey {
    public:
        /**
         * Returns the underlying byte array represented by this ComparisonKey.
         *
         * The returned StringData may not outlive the ComparisonKey used to create it, since the
         * ComparisonKey owns the underlying byte array.
         */
        StringData getKeyData() const {
            return StringData(_key);
        }

    private:
        friend class CollatorInterface;

        ComparisonKey(std::string key) : _key(std::move(key)) {}

        std::string _key;
    };

    /**
     * Constructs a CollatorInterface capable of computing the collation described by 'spec'.
     */
    CollatorInterface(Collation spec) : _spec(std::move(spec)) {}

    virtual ~CollatorInterface() {}

    virtual std::unique_ptr<CollatorInterface> clone() const = 0;

    /**
     * Returns a number < 0 if 'left' is less than 'right' with respect to the collation, a number >
     * 0 if 'left' is greater than 'right' w.r.t. the collation, and 0 if 'left' and 'right' are
     * equal w.r.t. the collation.
     */
    virtual int compare(StringData left, StringData right) const = 0;

    /**
     * Hashes the string such that strings which are equal under this collation also have equal
     * hashes.
     */
    void hash_combine(size_t& seed, StringData stringToHash) const final;

    /**
     * Returns the comparison key for 'stringData', according to this collation. See ComparisonKey's
     * comments for details.
     */
    virtual ComparisonKey getComparisonKey(StringData stringData) const = 0;

    /**
     * Returns the comparison key string for 'stringData', according to this collation. See
     * ComparisonKey's comments for details.
     */
    std::string getComparisonString(StringData stringData) const;

    /**
     * Returns whether this collation has the same matching and sorting semantics as 'other'.
     */
    bool operator==(const CollatorInterface& other) const {
        return getSpec() == other.getSpec();
    }

    /**
     * Returns whether this collation *does not* have the same matching and sorting semantics as
     * 'other'.
     */
    bool operator!=(const CollatorInterface& other) const {
        return !(*this == other);
    }

    /**
     * Returns a reference to the Collation.
     */
    const Collation& getSpec() const {
        return _spec;
    }

    /**
     * Returns true if lhs and rhs are both nullptr, or if they point to equivalent collators.
     */
    static bool collatorsMatch(const CollatorInterface* lhs, const CollatorInterface* rhs) {
        if (lhs == nullptr && rhs == nullptr) {
            return true;
        }
        if (lhs == nullptr || rhs == nullptr) {
            return false;
        }
        return (*lhs == *rhs);
    }

    /**
     * Returns a clone of 'collator'. If 'collator' is nullptr, returns the null collator.
     */
    static std::unique_ptr<CollatorInterface> cloneCollator(const CollatorInterface* collator) {
        if (!collator) {
            return {nullptr};
        }
        return collator->clone();
    }

protected:
    static ComparisonKey makeComparisonKey(std::string key) {
        return ComparisonKey(std::move(key));
    }

private:
    const Collation _spec;
};

}  // namespace mongo
