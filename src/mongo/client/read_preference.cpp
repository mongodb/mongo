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
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kModeFieldName[] = "mode";
const char kTagsFieldName[] = "tags";
const char kMaxStalenessMSFieldName[] = "maxStalenessMS";

const char kPrimaryOnly[] = "primary";
const char kPrimaryPreferred[] = "primaryPreferred";
const char kSecondaryOnly[] = "secondary";
const char kSecondaryPreferred[] = "secondaryPreferred";
const char kNearest[] = "nearest";

StringData readPreferenceName(ReadPreference pref) {
    switch (pref) {
        case ReadPreference::PrimaryOnly:
            return StringData(kPrimaryOnly);
        case ReadPreference::PrimaryPreferred:
            return StringData(kPrimaryPreferred);
        case ReadPreference::SecondaryOnly:
            return StringData(kSecondaryOnly);
        case ReadPreference::SecondaryPreferred:
            return StringData(kSecondaryPreferred);
        case ReadPreference::Nearest:
            return StringData(kNearest);
        default:
            MONGO_UNREACHABLE;
    }
}

StatusWith<ReadPreference> parseReadPreferenceMode(StringData prefStr) {
    if (prefStr == kPrimaryOnly) {
        return ReadPreference::PrimaryOnly;
    } else if (prefStr == kPrimaryPreferred) {
        return ReadPreference::PrimaryPreferred;
    } else if (prefStr == kSecondaryOnly) {
        return ReadPreference::SecondaryOnly;
    } else if (prefStr == kSecondaryPreferred) {
        return ReadPreference::SecondaryPreferred;
    } else if (prefStr == kNearest) {
        return ReadPreference::Nearest;
    }
    return Status(ErrorCodes::FailedToParse,
                  str::stream() << "Could not parse $readPreference mode '" << prefStr
                                << "'. Only the modes '"
                                << kPrimaryOnly
                                << "', '"
                                << kPrimaryPreferred
                                << "', "
                                << kSecondaryOnly
                                << "', '"
                                << kSecondaryPreferred
                                << "', and '"
                                << kNearest
                                << "' are supported.");
}

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

TagSet::TagSet() : _tags(BSON_ARRAY(BSONObj())) {}

TagSet TagSet::primaryOnly() {
    return TagSet{BSONArray()};
}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref,
                                             TagSet tags,
                                             Milliseconds maxStalenessMS)
    : pref(std::move(pref)), tags(std::move(tags)), maxStalenessMS(std::move(maxStalenessMS)) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref, TagSet tags)
    : pref(std::move(pref)), tags(std::move(tags)) {}

ReadPreferenceSetting::ReadPreferenceSetting(ReadPreference pref)
    : ReadPreferenceSetting(pref, defaultTagSetForMode(pref)) {}

StatusWith<ReadPreferenceSetting> ReadPreferenceSetting::fromBSON(const BSONObj& readPrefObj) {
    std::string modeStr;
    auto modeExtractStatus = bsonExtractStringField(readPrefObj, kModeFieldName, &modeStr);
    if (!modeExtractStatus.isOK()) {
        return modeExtractStatus;
    }

    ReadPreference mode;
    auto swReadPrefMode = parseReadPreferenceMode(modeStr);
    if (!swReadPrefMode.isOK()) {
        return swReadPrefMode.getStatus();
    }
    mode = std::move(swReadPrefMode.getValue());

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

    long long maxStalenessMSValue;
    auto maxStalenessMSExtractStatus = bsonExtractIntegerFieldWithDefault(
        readPrefObj, kMaxStalenessMSFieldName, 0, &maxStalenessMSValue);

    if (!maxStalenessMSExtractStatus.isOK()) {
        return maxStalenessMSExtractStatus;
    }

    if (maxStalenessMSValue < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kMaxStalenessMSFieldName
                                    << " must be a non negative integer");
    }

    if (maxStalenessMSValue >= Milliseconds::max().count()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kMaxStalenessMSFieldName << " value can not exceed"
                                    << Milliseconds::max().count());
    }

    return ReadPreferenceSetting(mode, tags, Milliseconds(maxStalenessMSValue));
}

BSONObj ReadPreferenceSetting::toBSON() const {
    BSONObjBuilder bob;
    bob.append(kModeFieldName, readPreferenceName(pref));
    if (tags != defaultTagSetForMode(pref)) {
        bob.append(kTagsFieldName, tags.getTagBSON());
    }
    if (maxStalenessMS.count() > 0) {
        bob.append(kMaxStalenessMSFieldName, maxStalenessMS.count());
    }
    return bob.obj();
}

std::string ReadPreferenceSetting::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
