// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * An interface for ordering and matching according to a collation. Instances should be retrieved
 * from the CollatorFactoryInterface and may not be copied.
 *
 * All methods are thread-safe.
 *
 * Does not throw exceptions.
 */

class CollatorInterface;

constexpr CollatorInterface* kSimpleCollator = nullptr;

class CollatorInterface : public StringDataComparator {
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
         * The returned std::string_view may not outlive the ComparisonKey used to create it, since
         * the ComparisonKey owns the underlying byte array.
         */
        std::string_view getKeyData() const {
            return std::string_view(_key);
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

    ~CollatorInterface() override {}

    virtual std::unique_ptr<CollatorInterface> clone() const = 0;
    virtual std::shared_ptr<CollatorInterface> cloneShared() const = 0;

    /**
     * Returns a number < 0 if 'left' is less than 'right' with respect to the collation, a number >
     * 0 if 'left' is greater than 'right' w.r.t. the collation, and 0 if 'left' and 'right' are
     * equal w.r.t. the collation.
     */
    int compare(std::string_view left, std::string_view right) const override = 0;

    /**
     * Hashes the string such that strings which are equal under this collation also have equal
     * hashes.
     */
    void hash_combine(size_t& seed, std::string_view stringToHash) const final;

    /**
     * Returns the comparison key for 'stringData', according to this collation. See ComparisonKey's
     * comments for details.
     */
    virtual ComparisonKey getComparisonKey(std::string_view stringData) const = 0;

    /**
     * Returns the comparison key string for 'stringData', according to this collation. See
     * ComparisonKey's comments for details.
     */
    std::string getComparisonString(std::string_view stringData) const;

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
     * Returns true if lhs and rhs are both the simple collator (nullptr), or if they point to
     * equivalent collators.
     */
    static bool collatorsMatch(const CollatorInterface* lhs, const CollatorInterface* rhs) {
        if (isSimpleCollator(lhs) && isSimpleCollator(rhs)) {
            return true;
        }
        if (isSimpleCollator(lhs) || isSimpleCollator(rhs)) {
            return false;
        }
        return (*lhs == *rhs);
    }

    /**
     * Returns a clone of 'collator'.
     * If 'collator' is the simple collator (nullptr), returns it again.
     */
    static std::unique_ptr<CollatorInterface> cloneCollator(const CollatorInterface* collator) {
        if (isSimpleCollator(collator)) {
            return std::unique_ptr<CollatorInterface>{kSimpleCollator};
        }
        return collator->clone();
    }

    static bool isSimpleCollator(const CollatorInterface* collator) {
        return collator == kSimpleCollator;
    }

protected:
    static ComparisonKey makeComparisonKey(std::string key) {
        return ComparisonKey(std::move(key));
    }

private:
    const Collation _spec;
};

}  // namespace mongo
