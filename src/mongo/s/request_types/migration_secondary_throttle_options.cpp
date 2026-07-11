// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/request_types/migration_secondary_throttle_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/write_concern_options.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const char kSecondaryThrottleMongos[] = "_secondaryThrottle";
const char kSecondaryThrottleMongod[] = "secondaryThrottle";
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
    if (holds_alternative<int64_t>(writeConcern.w) && get<int64_t>(writeConcern.w) <= 1) {
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
            bsonExtractBooleanField(obj, kSecondaryThrottleMongod, &isSecondaryThrottle);
        if (status == ErrorCodes::NoSuchKey) {
            status = bsonExtractBooleanField(obj, kSecondaryThrottleMongos, &isSecondaryThrottle);
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
            // Ignore the specified writeConcern, since it won't be used.  This is necessary
            // to normalize the otherwise non-standard way that moveChunk uses writeConcern (ie.
            // only using it when secondaryThrottle: true), so that shardsvrs can enforce always
            // receiving writeConcern on internalClient connections (at the ServiceEntryPoint
            // layer).
            return MigrationSecondaryThrottleOptions(secondaryThrottle, boost::none);
        }

        writeConcernBSON = writeConcernElem.Obj().getOwned();
    }

    invariant(writeConcernBSON.has_value());

    // Make sure the write concern parses correctly
    auto sw = WriteConcernOptions::parse(*writeConcernBSON);
    if (!sw.isOK()) {
        return sw.getStatus();
    }

    return MigrationSecondaryThrottleOptions(secondaryThrottle, std::move(writeConcernBSON));
}

StatusWith<MigrationSecondaryThrottleOptions>
MigrationSecondaryThrottleOptions::createFromBalancerConfig(const BSONElement& elem) {
    if (elem.eoo()) {
        return MigrationSecondaryThrottleOptions::create(kDefault);
    }

    if (elem.type() == BSONType::boolean) {
        return MigrationSecondaryThrottleOptions::create(elem.Bool() ? kOn : kOff);
    } else if (elem.type() == BSONType::object) {
        auto sw = WriteConcernOptions::parse(elem.Obj());
        if (!sw.isOK()) {
            return sw.getStatus();
        }
        return MigrationSecondaryThrottleOptions::createWithWriteConcern(sw.getValue());
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected bool or object for _secondaryThrottle, got: "
                                    << typeName(elem.type()));
    }
}

WriteConcernOptions MigrationSecondaryThrottleOptions::getWriteConcern() const {
    invariant(_secondaryThrottle != kOff);
    invariant(_writeConcernBSON);

    return fassert(34414, WriteConcernOptions::parse(*_writeConcernBSON));
}

void MigrationSecondaryThrottleOptions::append(BSONObjBuilder* builder) const {
    if (_secondaryThrottle == kDefault) {
        return;
    }

    builder->appendBool(kSecondaryThrottleMongod, _secondaryThrottle == kOn);

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

MigrationSecondaryThrottleOptions MigrationSecondaryThrottleOptions::parseFromBalancerConfigElement(
    const BSONElement& element) {
    auto sw = MigrationSecondaryThrottleOptions::createFromBalancerConfig(element);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Invalid MigrationSecondaryThrottle: " << sw.getStatus(),
            sw.isOK());
    return sw.getValue();
}

void MigrationSecondaryThrottleOptions::serializeToBalancerConfigElement(
    std::string_view fieldName, BSONObjBuilder* builder) const {

    if (_secondaryThrottle == kDefault) {
        return;
    }

    if (_secondaryThrottle == kOff) {
        builder->appendBool(fieldName, false);
    } else if (_secondaryThrottle == kOn) {
        if (_writeConcernBSON.has_value()) {
            builder->append(fieldName, *_writeConcernBSON);
        } else {
            builder->appendBool(fieldName, true);
        }
    }
}


}  // namespace mongo
