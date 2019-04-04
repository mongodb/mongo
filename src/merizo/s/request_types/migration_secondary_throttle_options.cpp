/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/s/request_types/migration_secondary_throttle_options.h"

#include "merizo/base/status_with.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/db/write_concern_options.h"

namespace merizo {
namespace {

const char kSecondaryThrottleMerizos[] = "_secondaryThrottle";
const char kSecondaryThrottleMerizod[] = "secondaryThrottle";
const char kWriteConcern[] = "writeConcern";

}  // namespace

MigrationSecondaryThrottleOptions::MigrationSecondaryThrottleOptions(
    SecondaryThrottleOption secondaryThrottle, boost::optional<BSONObj> writeConcernBSON)
    : _secondaryThrottle(secondaryThrottle), _writeConcernBSON(std::move(writeConcernBSON)) {}

MigrationSecondaryThrottleOptions MigrationSecondaryThrottleOptions::create(
    SecondaryThrottleOption option) {
    return MigrationSecondaryThrottleOptions(option, boost::none);
}

MigrationSecondaryThrottleOptions MigrationSecondaryThrottleOptions::createWithWriteConcern(
    const WriteConcernOptions& writeConcern) {
    // Optimize on write concern, which makes no difference
    if (writeConcern.wNumNodes <= 1 && writeConcern.wMode.empty()) {
        return MigrationSecondaryThrottleOptions(kOff, boost::none);
    }

    return MigrationSecondaryThrottleOptions(kOn, writeConcern.toBSON());
}

StatusWith<MigrationSecondaryThrottleOptions> MigrationSecondaryThrottleOptions::createFromCommand(
    const BSONObj& obj) {
    SecondaryThrottleOption secondaryThrottle;
    boost::optional<BSONObj> writeConcernBSON;

    // Parse the two variants of the 'secondaryThrottle' option
    {
        bool isSecondaryThrottle;

        Status status =
            bsonExtractBooleanField(obj, kSecondaryThrottleMerizod, &isSecondaryThrottle);
        if (status == ErrorCodes::NoSuchKey) {
            status = bsonExtractBooleanField(obj, kSecondaryThrottleMerizos, &isSecondaryThrottle);
        }

        if (status == ErrorCodes::NoSuchKey) {
            secondaryThrottle = kDefault;
        } else if (status.isOK()) {
            secondaryThrottle = (isSecondaryThrottle ? kOn : kOff);
        } else {
            return status;
        }
    }

    // Extract the requested 'writeConcern' option
    {
        BSONElement writeConcernElem;
        Status status = bsonExtractField(obj, kWriteConcern, &writeConcernElem);
        if (status == ErrorCodes::NoSuchKey) {
            return MigrationSecondaryThrottleOptions(secondaryThrottle, boost::none);
        } else if (!status.isOK()) {
            return status;
        }

        if (secondaryThrottle != kOn) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Cannot specify write concern when secondaryThrottle is not set");
        }

        writeConcernBSON = writeConcernElem.Obj().getOwned();
    }

    invariant(writeConcernBSON.is_initialized());

    // Make sure the write concern parses correctly
    WriteConcernOptions writeConcern;
    Status status = writeConcern.parse(*writeConcernBSON);
    if (!status.isOK()) {
        return status;
    }

    return MigrationSecondaryThrottleOptions(secondaryThrottle, std::move(writeConcernBSON));
}

StatusWith<MigrationSecondaryThrottleOptions>
MigrationSecondaryThrottleOptions::createFromBalancerConfig(const BSONObj& obj) {
    {
        bool isSecondaryThrottle;
        Status status =
            bsonExtractBooleanField(obj, kSecondaryThrottleMerizos, &isSecondaryThrottle);
        if (status.isOK()) {
            return MigrationSecondaryThrottleOptions::create(isSecondaryThrottle ? kOn : kOff);
        } else if (status == ErrorCodes::NoSuchKey) {
            return MigrationSecondaryThrottleOptions::create(kDefault);
        } else if (status != ErrorCodes::TypeMismatch) {
            return status;
        }
    }

    // Try to load it as a BSON document
    BSONElement elem;
    Status status = bsonExtractTypedField(obj, kSecondaryThrottleMerizos, BSONType::Object, &elem);
    if (!status.isOK())
        return status;

    WriteConcernOptions writeConcern;
    Status writeConcernParseStatus = writeConcern.parse(elem.Obj());
    if (!writeConcernParseStatus.isOK()) {
        return writeConcernParseStatus;
    }

    return MigrationSecondaryThrottleOptions::createWithWriteConcern(writeConcern);
}

WriteConcernOptions MigrationSecondaryThrottleOptions::getWriteConcern() const {
    invariant(_secondaryThrottle != kOff);
    invariant(_writeConcernBSON);

    WriteConcernOptions writeConcern;
    fassert(34414, writeConcern.parse(*_writeConcernBSON));

    return writeConcern;
}

void MigrationSecondaryThrottleOptions::append(BSONObjBuilder* builder) const {
    if (_secondaryThrottle == kDefault) {
        return;
    }

    builder->appendBool(kSecondaryThrottleMerizod, _secondaryThrottle == kOn);

    if (_secondaryThrottle == kOn && _writeConcernBSON) {
        builder->append(kWriteConcern, *_writeConcernBSON);
    }
}

BSONObj MigrationSecondaryThrottleOptions::toBSON() const {
    BSONObjBuilder builder;
    append(&builder);
    return builder.obj();
}

bool MigrationSecondaryThrottleOptions::operator==(
    const MigrationSecondaryThrottleOptions& other) const {
    return toBSON().woCompare(other.toBSON()) == 0;
}

bool MigrationSecondaryThrottleOptions::operator!=(
    const MigrationSecondaryThrottleOptions& other) const {
    return !(*this == other);
}

}  // namespace merizo
