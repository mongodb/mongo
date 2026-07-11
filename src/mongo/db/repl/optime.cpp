// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/optime.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/optime_base_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::repl {

// static
OpTime OpTime::max() {
    return OpTime(Timestamp::max(), std::numeric_limits<long long>::max());
}

void OpTime::append(std::string_view fieldName, BSONObjBuilder* builder) const {
    BSONObjBuilder opTimeBuilder(builder->subobjStart(fieldName));
    opTimeBuilder.append(kTimestampFieldName, _timestamp);
    opTimeBuilder.append(kTermFieldName, _term);
    opTimeBuilder.doneFast();
}

StatusWith<OpTime> OpTime::parseFromOplogEntry(const BSONObj& obj) {
    try {
        OpTimeBase base = OpTimeBase::parse(obj);
        long long term = base.getTerm().value_or(kUninitializedTerm);
        return OpTime(base.getTimestamp(), term);
    } catch (...) {
        return exceptionToStatus();
    }
}

BSONObj OpTime::toBSON() const {
    BSONObjBuilder bldr;
    bldr.append(kTimestampFieldName, _timestamp);
    bldr.append(kTermFieldName, _term);
    return bldr.obj();
}

// static
OpTime OpTime::parse(const BSONObj& obj) {
    return uassertStatusOK(parseFromOplogEntry(obj));
}

OpTime OpTime::parse(const BSONElement& elem) {
    return OpTime::parse(elem.Obj());
}

std::string OpTime::toString() const {
    return toBSON().toString();
}

std::ostream& operator<<(std::ostream& out, const OpTime& opTime) {
    return out << opTime.toString();
}

std::ostream& operator<<(std::ostream& out, const OpTimeAndWallTime& opTime) {
    return out << opTime.opTime.toString() << ", " << opTime.wallTime.toString();
}

void OpTime::appendAsQuery(BSONObjBuilder* builder) const {
    builder->append(kTimestampFieldName, _timestamp);
    if (_term == kUninitializedTerm) {
        fassertFailedWithStatus(7356000, Status(ErrorCodes::BadValue, toString()));
    }
    builder->append(kTermFieldName, _term);
}

BSONObj OpTime::asQuery() const {
    BSONObjBuilder builder;
    appendAsQuery(&builder);
    return builder.obj();
}

StatusWith<OpTimeAndWallTime> OpTimeAndWallTime::parseOpTimeAndWallTimeFromOplogEntry(
    const BSONObj& obj) {

    try {
        auto base = OpTimeAndWallTimeBase::parse(obj);

        auto opTime =
            OpTime(base.getTimestamp(), base.getTerm().value_or(OpTime::kUninitializedTerm));

        return OpTimeAndWallTime(opTime, base.getWall());
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

OpTimeAndWallTime OpTimeAndWallTime::parse(const BSONObj& obj) {
    return uassertStatusOK(parseOpTimeAndWallTimeFromOplogEntry(obj));
}

}  // namespace mongo::repl
