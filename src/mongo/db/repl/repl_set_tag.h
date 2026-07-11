// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {
class BSONObjBuilder;

namespace repl {

/**
 * Representation of a tag on a replica set node.
 *
 * Tags are only meaningful when used with a copy of the ReplSetTagConfig that
 * created them.
 */
class ReplSetTag {
public:
    /**
     * Default constructor, produces an uninitialized tag.
     */
    ReplSetTag() {}

    /**
     * Constructs a tag with the given key and value indexes.
     * Do not call directly; used by ReplSetTagConfig.
     */
    ReplSetTag(int32_t keyIndex, int32_t valueIndex)
        : _keyIndex(keyIndex), _valueIndex(valueIndex) {}

    /**
     * Returns true if the tag is not explicitly invalid.
     */
    bool isValid() const {
        return _keyIndex >= 0;
    }

    /**
     * Gets the key index of the tag.
     */
    int32_t getKeyIndex() const {
        return _keyIndex;
    }

    /**
     * Gets the value index of the tag.
     */
    int32_t getValueIndex() const {
        return _valueIndex;
    }

    /**
     * Compares two tags from the *same* ReplSetTagConfig for equality.
     */
    bool operator==(const ReplSetTag& other) const;

    /**
     * Compares two tags from the *same* ReplSetTagConfig for inequality.
     */
    bool operator!=(const ReplSetTag& other) const;

private:
    // The index of the key in the associated ReplSetTagConfig.
    int32_t _keyIndex;

    // The index of the value in the entry for the key in the associated ReplSetTagConfig.
    int32_t _valueIndex;
};

/**
 * Representation of a tag matching pattern, like { "dc": 2, "rack": 3 }, of the form
 * used for tagged replica set writes.
 */
class ReplSetTagPattern {
public:
    /**
     * Representation of a single tag's minimum count constraint in a pattern.
     */
    class TagCountConstraint {
    public:
        TagCountConstraint() {}
        TagCountConstraint(int32_t keyIndex, int32_t minCount);
        bool operator==(const TagCountConstraint& other) const {
            return _keyIndex == other._keyIndex && _minCount == other._minCount;
        }
        int32_t getKeyIndex() const {
            return _keyIndex;
        }
        int32_t getMinCount() const {
            return _minCount;
        }

    private:
        int32_t _keyIndex;
        int32_t _minCount;
    };

    typedef std::vector<TagCountConstraint>::const_iterator ConstraintIterator;

    /**
     * Adds a count constraint for the given key index with the given count.
     *
     * Do not call directly, but use the addTagCountConstraintToPattern method
     * of ReplSetTagConfig.
     */
    void addTagCountConstraint(int32_t keyIndex, int32_t minCount);

    /**
     * Gets the begin iterator over the constraints in this pattern.
     */
    ConstraintIterator constraintsBegin() const {
        return _constraints.begin();
    }

    /**
     * Gets the end iterator over the constraints in this pattern.
     */
    ConstraintIterator constraintsEnd() const {
        return _constraints.end();
    }

    /**
     * Gets the number of constraints in this pattern.
     */
    size_t getNumConstraints() const {
        return _constraints.size();
    }

    bool operator==(const ReplSetTagPattern& other) const {
        if (getNumConstraints() != other.getNumConstraints()) {
            return false;
        }

        for (auto itrA = constraintsBegin(); itrA != constraintsEnd(); itrA++) {
            bool same = false;
            for (auto itrB = other.constraintsBegin(); itrB != other.constraintsEnd(); itrB++) {
                if (*itrA == *itrB) {
                    same = true;
                    break;
                }
            }
            if (!same) {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const ReplSetTagPattern& other) const {
        return !operator==(other);
    }

private:
    std::vector<TagCountConstraint> _constraints;
};

/**
 * State object for progressive detection of ReplSetTagPattern constraint satisfaction.
 *
 * This is an abstraction of the replica set write tag satisfaction problem.
 *
 * Replica set tag matching is an event-driven constraint satisfaction process.  This type
 * represents the state of that process.  It is initialized from a pattern object, then
 * progressively updated with tags.  After processing a sequence of tags sufficient to satisfy
 * the pattern, isSatisfied() becomes true.
 */
class ReplSetTagMatch {
    friend class ReplSetTagConfig;

public:
    /**
     * Constructs an empty match object, equivalent to one that matches an
     * empty pattern.
     */
    ReplSetTagMatch() {}

    /**
     * Constructs a clean match object for the given pattern.
     */
    explicit ReplSetTagMatch(const ReplSetTagPattern& pattern);

    /**
     * Updates the match state based on the data for the given tag.
     *
     * Returns true if, after this update, isSatisfied() is true.
     */
    bool update(const ReplSetTag& tag);

    /**
     * Returns true if the match has received a sequence of tags sufficient to satisfy the
     * pattern.
     */
    bool isSatisfied() const;

private:
    /**
     * Representation of the state related to a single tag key in the match pattern.
     * Consists of a constraint (key index and min count for satisfaction) and a list
     * of already observed values.
     *
     * A BoundTagValue is satisfied when the size of boundValues is at least
     * constraint.getMinCount().
     */
    struct BoundTagValue {
        BoundTagValue() {}
        explicit BoundTagValue(const ReplSetTagPattern::TagCountConstraint& aConstraint)
            : constraint(aConstraint) {}

        int32_t getKeyIndex() const {
            return constraint.getKeyIndex();
        }
        bool isSatisfied() const;

        ReplSetTagPattern::TagCountConstraint constraint;
        std::vector<int32_t> boundValues;
    };
    std::vector<BoundTagValue> _boundTagValues;
};

/**
 * Representation of the tag configuration information for a replica set.
 *
 * This type, like all in this file, is copyable.  Tags and patterns from one instance of this
 * class are compatible with other instances of this class that are *copies* of the original
 * instance.
 */
class [[MONGO_MOD_PUBLIC]] ReplSetTagConfig {
public:
    /**
     * Finds or allocates a tag with the given "key" and "value" strings.
     */
    ReplSetTag makeTag(std::string_view key, std::string_view value);

    /**
     * Finds a tag with the given key and value strings, or returns a tag whose isValid() method
     * returns false if the configuration has never allocated such a tag via makeTag().
     */
    ReplSetTag findTag(std::string_view key, std::string_view value) const;

    /**
     * Makes a new, empty pattern object.
     */
    ReplSetTagPattern makePattern() const;

    /**
     * Adds a constraint clause to the given "pattern".  This particular
     * constraint requires that at least "minCount" distinct tags with the given "tagKey"
     * be observed.  Two tags "t1" and "t2" are distinct if "t1 != t2", so this constraint
     * means that we must see at least "minCount" tags with the specified "tagKey".
     */
    Status addTagCountConstraintToPattern(ReplSetTagPattern* pattern,
                                          std::string_view tagKey,
                                          int32_t minCount) const;

    /**
     * Gets the string key for the given "tag".
     *
     * Behavior is undefined if "tag" is not valid or was not from this
     * config or one of its copies.
     */
    std::string getTagKey(const ReplSetTag& tag) const;

    /**
     * Gets the string value for the given "tag".
     *
     * Like getTagKey, above, behavior is undefined if "tag" is not valid or was not from this
     * config or one of its copies.
     */
    std::string getTagValue(const ReplSetTag& tag) const;

    /**
     * Helper that writes a string debugging representation of "tag" to "os".
     */
    void put(const ReplSetTag& tag, std::ostream& os) const;

    /**
     * Helper that writes a string debugging representation of "pattern" to "os".
     */
    void put(const ReplSetTagPattern& pattern, std::ostream& os) const;

    /**
     * Helper that writes a string debugging representation of "matcher" to "os".
     */
    void put(const ReplSetTagMatch& matcher, std::ostream& os) const;

private:
    typedef std::vector<std::string> ValueVector;
    typedef std::vector<std::pair<std::string, ValueVector>> KeyValueVector;

    /**
     * Returns the index corresponding to "key", or _tagData.size() if there is no
     * such index.
     */
    int32_t _findKeyIndex(std::string_view key) const;

    /**
     * Helper that writes a "tagKey" field for the given "keyIndex" to "builder".
     */
    void _appendTagKey(int32_t keyIndex, BSONObjBuilder* builder) const;

    /**
     * Helper that writes a "tagValue" field for the given "keyIndex" and "valueIndex"
     * to "builder".
     */
    void _appendTagValue(int32_t keyIndex, int32_t valueIndex, BSONObjBuilder* builder) const;

    /**
     * Helper that writes a constraint object to "builder".
     */
    void _appendConstraint(const ReplSetTagPattern::TagCountConstraint& constraint,
                           BSONObjBuilder* builder) const;

    // Data about known tags.  Conceptually, it maps between keys and their indexes,
    // keys and their associated values, and (key, value) pairs and the values' indexes.
    KeyValueVector _tagData;
};

}  // namespace repl
}  // namespace mongo
