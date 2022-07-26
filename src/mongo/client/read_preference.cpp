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

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

const char kModeFieldName[] = "mode";
const char kTagsFieldName[] = "tags";
const char kMaxStalenessSecondsFieldName[] = "maxStalenessSeconds";
const char kHedgeFieldName[] = "hedge";

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

Status validateReadPreferenceMode(const std::string& prefStr) {
    try {
        ReadPreference_parse(IDLParserContext(kModeFieldName), prefStr);
    } catch (DBException& e) {
        return e.toStatus();
    }
    return Status::OK();
}

/**
 * Replica set refresh period on the task executor.
 */
const Seconds ReadPreferenceSetting::kMinimalMaxStalenessValue(90);

const OperationContext::Decoration<ReadPreferenceSetting> ReadPreferenceSetting::get =
    OperationContext::declareDecoration<ReadPreferenceSetting>();

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
                                             boost::optional<HedgingMode> hedgingMode)
    : pref(std::move(pref)),
      tags(std::move(tags)),
      maxStalenessSeconds(std::move(maxStalenessSeconds)),
      hedgingMode(std::move(hedgingMode)) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref, Seconds maxStalenessSeconds)
    : ReadPreferenceSetting(pref, defaultTagSetForMode(pref), maxStalenessSeconds) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref, TagSet tags)
    : pref(std::move(pref)), tags(std::move(tags)) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref)
    : ReadPreferenceSetting(pref, defaultTagSetForMode(pref)) {}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromInnerBSON(const BSONObj& readPrefObj) {
    std::string modeStr;
    auto modeExtractStatus = bsonExtractStringField(readPrefObj, kModeFieldName, &modeStr);
    if (!modeExtractStatus.isOK()) {
        return modeExtractStatus;
    }

    ReadPreference mode;
    try {
        mode = ReadPreference_parse(IDLParserContext(kModeFieldName), modeStr);
    } catch (DBException& e) {
        return e.toStatus().withContext(
            str::stream() << "Could not parse $readPreference mode '" << modeStr
                          << "'. Only the modes '"
                          << ReadPreference_serializer(ReadPreference::PrimaryOnly) << "', '"
                          << ReadPreference_serializer(ReadPreference::PrimaryPreferred) << "', '"
                          << ReadPreference_serializer(ReadPreference::SecondaryOnly) << "', '"
                          << ReadPreference_serializer(ReadPreference::SecondaryPreferred)
                          << "', and '" << ReadPreference_serializer(ReadPreference::Nearest)
                          << "' are supported.");
    }

    boost::optional<HedgingMode> hedgingMode;
    if (auto hedgingModeEl = readPrefObj[kHedgeFieldName]) {
        if (hedgingModeEl.type() != BSONType::Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << kHedgeFieldName
                                        << " field must be of type object if provided; found "
                                        << hedgingModeEl);
        }
        hedgingMode =
            HedgingMode::parse(IDLParserContext(kHedgeFieldName), hedgingModeEl.embeddedObject());
        if (hedgingMode->getEnabled() && mode == ReadPreference::PrimaryOnly) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "cannot enable hedging for $readPreference mode \"primaryOnly\""};
        }
    }

    if (!hedgingMode && mode == ReadPreference::Nearest) {
        hedgingMode = HedgingMode();
    }

    TagSet tags;
    BSONElement tagsElem;
    auto tagExtractStatus =
        bsonExtractTypedField(readPrefObj, kTagsFieldName, mongo::Array, &tagsElem);
    if (tagExtractStatus.isOK()) {
        tags = TagSet{BSONArray(tagsElem.Obj().getOwned())};

        // In accordance with the read preference spec, passing the default wildcard tagset
        // '[{}]' is the same as not passing a TagSet at all. Furthermore, passing an empty
        // TagSet with a non-primary ReadPreference is equivalent to passing the wildcard
        // ReadPreference.
        if (tags == TagSet() || tags == TagSet::primaryOnly()) {
            tags = defaultTagSetForMode(mode);
        }

        // If we are using a user supplied TagSet, check that it is compatible with
        // the readPreference mode.
        else if (ReadPreference::PrimaryOnly == mode && (tags != TagSet::primaryOnly())) {
            return Status(ErrorCodes::BadValue,
                          "Only empty tags are allowed with primary read preference");
        }
    }

    else if (ErrorCodes::NoSuchKey == tagExtractStatus) {
        tags = defaultTagSetForMode(mode);
    } else {
        return tagExtractStatus;
    }

    long long maxStalenessSecondsValue;
    auto maxStalenessSecondsExtractStatus = bsonExtractIntegerFieldWithDefault(
        readPrefObj, kMaxStalenessSecondsFieldName, 0, &maxStalenessSecondsValue);

    if (!maxStalenessSecondsExtractStatus.isOK()) {
        return maxStalenessSecondsExtractStatus;
    }

    if (maxStalenessSecondsValue && maxStalenessSecondsValue < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << kMaxStalenessSecondsFieldName << " must be a non-negative integer");
    }

    if (maxStalenessSecondsValue && maxStalenessSecondsValue >= Seconds::max().count()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kMaxStalenessSecondsFieldName << " value can not exceed "
                                    << Seconds::max().count());
    }

    if (maxStalenessSecondsValue && maxStalenessSecondsValue < kMinimalMaxStalenessValue.count()) {
        return Status(ErrorCodes::MaxStalenessOutOfRange,
                      str::stream()
                          << kMaxStalenessSecondsFieldName << " value can not be less than "
                          << kMinimalMaxStalenessValue.count());
    }

    if ((mode == ReadPreference::PrimaryOnly) && maxStalenessSecondsValue) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kMaxStalenessSecondsFieldName
                                    << " can not be set for the primary mode");
    }

    return ReadPreferenceSetting(mode, tags, Seconds(maxStalenessSecondsValue), hedgingMode);
}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromInnerBSON(const BSONElement& elem) {
    if (elem.type() != mongo::Object) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "$readPreference has incorrect type: expected "
                                    << mongo::Object << " but got " << elem.type());
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

void ReadPreferenceSetting::toInnerBSON(BSONObjBuilder* bob) const {
    bob->append(kModeFieldName, ReadPreference_serializer(pref));
    if (tags != defaultTagSetForMode(pref)) {
        bob->append(kTagsFieldName, tags.getTagBSON());
    }
    if (maxStalenessSeconds.count() > 0) {
        bob->append(kMaxStalenessSecondsFieldName, maxStalenessSeconds.count());
    }
    if (hedgingMode) {
        bob->append(kHedgeFieldName, hedgingMode.get().toBSON());
    }
}

std::string ReadPreferenceSetting::toString() const {
    return toInnerBSON().toString();
}

}  // namespace mongo
