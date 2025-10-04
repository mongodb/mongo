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

#include "mongo/client/read_preference.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const char kModeFieldName[] = "mode";
const char kTagsFieldName[] = "tags";
const char kMaxStalenessSecondsFieldName[] = "maxStalenessSeconds";
const char kHedgeFieldName[] = "hedge";
const char kIsPretargetedFieldName[] = "$_isPretargeted";

// Slight kludge here: if we weren't passed a TagSet, we default to the empty
// TagSet if ReadPreference is Primary, or the default (wildcard) TagSet otherwise.
// This maintains compatibility with existing code, while preserving the ability to round
// trip.
TagSet defaultTagSetForMode(ReadPreference mode) {
    switch (mode) {
        case ReadPreference::PrimaryOnly:
            return TagSet::primaryOnly();
        default:
            return TagSet();
    }
}

}  // namespace

const BSONArray TagSet::kMatchAny = BSON_ARRAY(BSONObj());

Status validateReadPreferenceMode(const std::string& prefStr, const boost::optional<TenantId>&) {
    try {
        ReadPreference_parse(prefStr, IDLParserContext(kModeFieldName));
    } catch (DBException& e) {
        return e.toStatus();
    }
    return Status::OK();
}

const BSONObj& ReadPreferenceSetting::secondaryPreferredMetadata() {
    // This is a static method rather than a static member only because it is used by another TU
    // during dynamic init.
    static const auto bson =
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred).toContainingBSON();
    return bson;
}

TagSet::TagSet() : _tags(BSON_ARRAY(BSONObj())) {}

TagSet TagSet::primaryOnly() {
    return TagSet{BSONArray()};
}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref,
                                             TagSet tags,
                                             Seconds maxStalenessSeconds,
                                             boost::optional<HedgingMode> hedgingMode,
                                             bool isPretargeted)
    : pref(std::move(pref)),
      tags(std::move(tags)),
      maxStalenessSeconds(std::move(maxStalenessSeconds)),
      hedgingMode(std::move(hedgingMode)),
      isPretargeted(isPretargeted) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref, Seconds maxStalenessSeconds)
    : ReadPreferenceSetting(pref, defaultTagSetForMode(pref), maxStalenessSeconds) {
    _usedDefaultReadPrefValue = true;
}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref, TagSet tags)
    : pref(std::move(pref)), tags(std::move(tags)) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref)
    : ReadPreferenceSetting(pref, defaultTagSetForMode(pref)) {
    _usedDefaultReadPrefValue = true;
}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromReadPreferenceIdl(
    const ReadPreferenceIdl& rp) {
    boost::optional<HedgingMode> hedgingMode;
    if (rp.getHedge()) {
        hedgingMode = rp.getHedge();
        if (hedgingMode->getEnabled() && rp.getMode() == ReadPreference::PrimaryOnly) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "cannot enable hedging for $readPreference mode \"primaryOnly\""};
        }
    }

    TagSet tags;

    if (rp.getTags()) {
        tags = TagSet{static_cast<BSONArray>(rp.getTags()->getOwned())};

        // In accordance with the read preference spec, passing the default wildcard tagset
        // '[{}]' is the same as not passing a TagSet at all. Furthermore, passing an empty
        // TagSet with a non-primary ReadPreference is equivalent to passing the wildcard
        // ReadPreference.
        if (tags.isMatchAnyNode() || tags.isPrimaryOnly()) {
            tags = defaultTagSetForMode(rp.getMode());
        }

        // If we are using a user supplied TagSet, check that it is compatible with
        // the readPreference mode.
        else if (ReadPreference::PrimaryOnly == rp.getMode() && (!tags.isPrimaryOnly())) {
            return Status(ErrorCodes::BadValue,
                          "Only empty tags are allowed with primary read preference");
        }
    } else {
        tags = defaultTagSetForMode(rp.getMode());
    }

    auto maxStalenessSecondsValue = rp.getMaxStalenessSeconds().value_or(0);

    if ((rp.getMode() == ReadPreference::PrimaryOnly) && maxStalenessSecondsValue) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kMaxStalenessSecondsFieldName
                                    << " can not be set for the primary mode");
    }

    bool isPretargetedValue = false;
    if (auto isPretargeted = rp.getIsPretargeted()) {
        if (!isPretargeted.value()) {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << kIsPretargetedFieldName
                                        << " field must be true if provided; found "
                                        << isPretargeted.value());
        }
        isPretargetedValue = true;
    }

    return ReadPreferenceSetting(
        rp.getMode(), tags, Seconds(maxStalenessSecondsValue), hedgingMode, isPretargetedValue);
}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromInnerBSON(const BSONObj& readPrefObj) {
    ReadPreferenceIdl rp;

    // Translate any parsing related exceptions to status.
    try {
        rp = ReadPreferenceIdl::parse(readPrefObj, IDLParserContext("readPreference"));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return ReadPreferenceSetting::fromReadPreferenceIdl(rp);
}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromInnerBSON(const BSONElement& elem) {
    if (elem.type() != BSONType::object) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "$readPreference has incorrect type: expected "
                                    << BSONType::object << " but got " << elem.type());
    }
    return fromInnerBSON(elem.Obj());
}

ReadPreferenceSetting ReadPreferenceSetting::fromInnerBSONForIDL(const BSONObj& readPrefObj) {
    StatusWith<ReadPreferenceSetting> rps = fromInnerBSON(readPrefObj);
    uassertStatusOK(rps.getStatus());

    return rps.getValue();
}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromContainingBSON(
    const BSONObj& obj, ReadPreference defaultReadPref) {
    if (auto elem = obj["$readPreference"]) {
        return fromInnerBSON(elem);
    }
    return ReadPreferenceSetting(defaultReadPref);
}

ReadPreferenceIdl ReadPreferenceSetting::toReadPreferenceIdl() const {
    ReadPreferenceIdl rp;
    rp.setMode(pref);
    if (tags != defaultTagSetForMode(pref)) {
        rp.setTags(tags.getTagBSON());
    }
    if (maxStalenessSeconds.count() > 0) {
        rp.setMaxStalenessSeconds(maxStalenessSeconds.count());
    }
    rp.setHedge(hedgingMode);
    if (isPretargeted) {
        rp.setIsPretargeted(true);
    }
    return rp;
}

void ReadPreferenceSetting::toInnerBSON(BSONObjBuilder* bob) const {
    toReadPreferenceIdl().serialize(bob);
}

std::string ReadPreferenceSetting::toString() const {
    return toInnerBSON().toString();
}

}  // namespace mongo
