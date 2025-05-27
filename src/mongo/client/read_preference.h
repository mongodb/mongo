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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/hedging_mode_gen.h"
#include "mongo/client/read_preference_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/duration.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
template <typename T>
class StatusWith;

using ReadPreference = ReadPreferenceEnum;

/**
 * Validate a ReadPreference string. This is intended for use as an IDL validator callback.
 */
Status validateReadPreferenceMode(const std::string& prefStr, const boost::optional<TenantId>&);

/**
 * A simple object for representing the list of tags requested by a $readPreference.
 */
class TagSet {
private:
    static const BSONArray kMatchAny;

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

    /**
     * Primary only is defined as a empty BSON Array. See comments on primaryOnly().
     */
    bool isPrimaryOnly() const {
        return _tags.isEmpty();
    }

    /**
     * Match any node is defined as array with one element which is an empty BSON document. See
     * comments on default constructor.
     */
    bool isMatchAnyNode() const {
        return _tags.binaryEqual(kMatchAny);
    }

private:
    BSONArray _tags;
};

struct ReadPreferenceSetting {
    static const OperationContext::Decoration<ReadPreferenceSetting> get;

    /**
     * The minimal value maxStalenessSeconds can have.
     */
    static constexpr Seconds kMinimalMaxStalenessValue = Seconds(90);

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
    ReadPreferenceSetting(ReadPreference pref,
                          TagSet tags,
                          Seconds maxStalenessSeconds,
                          boost::optional<HedgingMode> hedgingMode = boost::none,
                          bool isPretargeted = false);
    ReadPreferenceSetting(ReadPreference pref, Seconds maxStalenessSeconds);
    ReadPreferenceSetting(ReadPreference pref, TagSet tags);
    explicit ReadPreferenceSetting(ReadPreference pref);
    ReadPreferenceSetting() : ReadPreferenceSetting(ReadPreference::PrimaryOnly) {
        _usedDefaultReadPrefValue = true;
    }

    inline bool equals(const ReadPreferenceSetting& other) const {
        auto hedgingModeEquals = [](const boost::optional<HedgingMode>& hedgingModeA,
                                    const boost::optional<HedgingMode>& hedgingModeB) -> bool {
            if (hedgingModeA && hedgingModeB) {
                return hedgingModeA->toBSON().woCompare(hedgingModeB->toBSON()) == 0;
            }
            return !hedgingModeA && !hedgingModeB;
        };

        return (pref == other.pref) && (tags == other.tags) &&
            (maxStalenessSeconds == other.maxStalenessSeconds) &&
            hedgingModeEquals(hedgingMode, other.hedgingMode) &&
            (minClusterTime == other.minClusterTime) && (isPretargeted == other.isPretargeted);
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
     * Returns the IDL-struct representation of this object's inner BSON serialized form.
     */
    ReadPreferenceIdl toReadPreferenceIdl() const;

    /**
     * Serializes this ReadPreferenceSetting as a containing BSON document. (The document containing
     * a $readPreference element)
     *
     * Will not add the $readPreference element if the read preference is PrimaryOnly.
     */
    void toContainingBSON(BSONObjBuilder* builder) const {
        if (!canRunOnSecondary() && !isPretargeted)
            return;  // Write nothing since default is fine.
        BSONObjBuilder inner(builder->subobjStart("$readPreference"));
        toInnerBSON(&inner);
    }
    BSONObj toContainingBSON() const {
        BSONObjBuilder bob;
        toContainingBSON(&bob);
        return bob.obj();
    }
    bool usedDefaultReadPrefValue() const {
        return _usedDefaultReadPrefValue;
    }
    /**
     * Parses a ReadPreferenceSetting from a BSON document of the form:
     * { mode: <mode>, tags: <array of tags>, maxStalenessSeconds: Number, hedge: <hedgingMode>}.
     * The 'mode' element must be a string equal to either "primary", "primaryPreferred",
     * "secondary", "secondaryPreferred", or "nearest". Although the tags array is intended to be an
     * array of unique BSON documents, no further validation is performed on it other than checking
     * that it is an array, and that it is empty if 'mode' is 'primary'. The 'hedge' element
     * consists of the optional field "enabled" (default true) and "delay" (default true).
     */
    static StatusWith<ReadPreferenceSetting> fromInnerBSON(const BSONObj& readPrefSettingObj);
    static StatusWith<ReadPreferenceSetting> fromInnerBSON(const BSONElement& readPrefSettingObj);

    /**
     * Parse a ReadPreferenceSetting from an IDL-struct representation of a read preference's inner
     * BSON document.
     *
     * This method validates the combination of fields specified on the provided IDL-struct.
     */
    static StatusWith<ReadPreferenceSetting> fromReadPreferenceIdl(const ReadPreferenceIdl& rp);

    /**
        Utilized by IDL types in order to get the unwrapped ReadPreferenceSetting object.
        It checks that the status is OK and then if so it will return the underlying object.
    */
    static ReadPreferenceSetting fromInnerBSONForIDL(const BSONObj& readPrefSettingObj);

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
    boost::optional<HedgingMode> hedgingMode;

    /**
     * Used by Server Selection to ensure that the Timestamp component of a node's current opTime
     * (ie. the opTime but ignoring the term) is at least this value.  Unless there are no known
     * nodes satisfying this condition, in which case it is ignored.
     *
     * It is valid to use ClusterTime values in minClusterTime because if a node has an opTime of X,
     * then that means it must have either:
     *
     * 1. Directly advanced the ClusterTime to X when doing that write as primary, or
     *
     * 2. Applied the op with that opTime after receiving it in a message from some other node; that
     *    message must (by the same recursive logic, if necessary) have gossiped a ClusterTime of at
     *    least X to this node.
     *
     * Either way, it must be that a node opTime of X implies ClusterTime >= X.
     */
    Timestamp minClusterTime{};

    // Set to true if this is a shardsvr mongod and the readPreference has been pre-targeted by
    // the client connected to it. Used by the replica set endpoint in sharding to mark commands
    // that it forces to go through the router as needing to target the local mongod.
    bool isPretargeted = false;

private:
    bool _usedDefaultReadPrefValue = false;
};

}  // namespace mongo
