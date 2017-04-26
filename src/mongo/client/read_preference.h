/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/duration.h"

namespace mongo {
template <typename T>
class StatusWith;

enum class ReadPreference {
    /**
     * Read from primary only. All operations produce an error (throw an exception where
     * applicable) if primary is unavailable. Cannot be combined with tags.
     */
    PrimaryOnly = 0,

    /**
     * Read from primary if available, otherwise a secondary. Tags will only be applied in the
     * event that the primary is unavailable and a secondary is read from. In this event only
     * secondaries matching the tags provided would be read from.
     */
    PrimaryPreferred,

    /**
     * Read from secondary if available, otherwise error.
     */
    SecondaryOnly,

    /**
     * Read from a secondary if available, otherwise read from the primary.
     */
    SecondaryPreferred,

    /**
     * Read from any member.
     */
    Nearest,
};

/**
 * A simple object for representing the list of tags requested by a $readPreference.
 */
class TagSet {
public:
    /**
     * Creates a TagSet that matches any nodes. This is the TagSet represented by the BSON
     * array containing a single empty document - [{}].
     *
     * Do not call during static init.
     */
    TagSet();

    /**
     * Returns an empty TagSet. This is the TagSet represented by the empty BSON array - [].
     * This TagSet must be associated with ReadPreference::PrimaryOnly.
     * ReadPreference::Primary.
     */
    static TagSet primaryOnly();

    /**
     * Creates a TagSet from a BSONArray of tags.
     *
     * @param tags the list of tags associated with this option. This object
     *     will get a shared copy of the list. Therefore, it is important
     *     for the the given tag to live longer than the created tag set.
     */
    explicit TagSet(const BSONArray& tags) : _tags(tags) {}

    /**
     * Returns the BSONArray listing all tags that should be accepted.
     */
    const BSONArray& getTagBSON() const {
        return _tags;
    }

    bool operator==(const TagSet& other) const {
        return SimpleBSONObjComparator::kInstance.evaluate(_tags == other._tags);
    }
    bool operator!=(const TagSet& other) const {
        return !(*this == other);
    }

private:
    BSONArray _tags;
};

struct ReadPreferenceSetting {
    static const OperationContext::Decoration<ReadPreferenceSetting> get;

    /**
     * The minimal value maxStalenessSeconds can have.
     */
    static const Seconds kMinimalMaxStalenessValue;

    /**
     * An object representing the metadata generated for a SecondaryPreferred read preference:
     * {$readPreference: {mode: "secondaryPreferred"}}
     */
    static const BSONObj& secondaryPreferredMetadata();

    /**
     * @param pref the read preference mode.
     * @param tag the tag set. Note that this object will have the
     *     tag set will have this in a reset state (meaning, this
     *     object's copy of tag will have the iterator in the initial
     *     position).
     */
    ReadPreferenceSetting(ReadPreference pref, TagSet tags, Seconds maxStalenessSeconds);
    ReadPreferenceSetting(ReadPreference pref, Seconds maxStalenessSeconds);
    ReadPreferenceSetting(ReadPreference pref, TagSet tags);
    explicit ReadPreferenceSetting(ReadPreference pref);
    ReadPreferenceSetting() : ReadPreferenceSetting(ReadPreference::PrimaryOnly) {}

    inline bool equals(const ReadPreferenceSetting& other) const {
        return (pref == other.pref) && (tags == other.tags) &&
            (maxStalenessSeconds == other.maxStalenessSeconds) && (minOpTime == other.minOpTime);
    }

    /**
     * Serializes this ReadPreferenceSetting as an inner BSON document. (The document inside a
     * $readPreference element)
     */
    void toInnerBSON(BSONObjBuilder* builder) const;
    BSONObj toInnerBSON() const {
        BSONObjBuilder bob;
        toInnerBSON(&bob);
        return bob.obj();
    }

    /**
     * Serializes this ReadPreferenceSetting as a containing BSON document. (The document containing
     * a $readPreference element)
     *
     * Will not add the $readPreference element if the read preference is PrimaryOnly.
     */
    void toContainingBSON(BSONObjBuilder* builder) const {
        if (!canRunOnSecondary())
            return;  // Write nothing since default is fine.
        BSONObjBuilder inner(builder->subobjStart("$readPreference"));
        toInnerBSON(&inner);
    }
    BSONObj toContainingBSON() const {
        BSONObjBuilder bob;
        toContainingBSON(&bob);
        return bob.obj();
    }

    /**
     * Parses a ReadPreferenceSetting from a BSON document of the form:
     * { mode: <mode>, tags: <array of tags>, maxStalenessSeconds: Number }. The 'mode' element must
     * be a string equal to either "primary", "primaryPreferred", "secondary", "secondaryPreferred",
     * or "nearest". Although the tags array is intended to be an array of unique BSON documents, no
     * further validation is performed on it other than checking that it is an array, and that it is
     * empty if 'mode' is 'primary'.
     */
    static StatusWith<ReadPreferenceSetting> fromInnerBSON(const BSONObj& readPrefSettingObj);
    static StatusWith<ReadPreferenceSetting> fromInnerBSON(const BSONElement& readPrefSettingObj);

    /**
     * Parses a ReadPreference setting from an object that may contain a $readPreference object
     * field with the contents described in fromInnerObject(). If the field is missing, returns the
     * default read preference.
     */
    static StatusWith<ReadPreferenceSetting> fromContainingBSON(
        const BSONObj& obj, ReadPreference defaultReadPref = ReadPreference::PrimaryOnly);

    /**
     * Describes this ReadPreferenceSetting as a string.
     */
    std::string toString() const;

    bool canRunOnSecondary() const {
        return pref != ReadPreference::PrimaryOnly;
    }

    ReadPreference pref;
    TagSet tags;
    Seconds maxStalenessSeconds{};
    repl::OpTime minOpTime{};
};

}  // namespace mongo
