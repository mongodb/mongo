// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/logical_time.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

LogicalTime::LogicalTime(Timestamp ts) : _time(ts.asULL()) {}

LogicalTime LogicalTime::fromOperationTime(const BSONObj& obj) {
    const auto opTimeElem(obj[kOperationTimeFieldName]);
    uassert(ErrorCodes::FailedToParse, "No operationTime found", !opTimeElem.eoo());
    uassert(ErrorCodes::BadValue,
            str::stream() << kOperationTimeFieldName << " is of the wrong type '"
                          << typeName(opTimeElem.type()) << "'",
            opTimeElem.type() == BSONType::timestamp);
    return LogicalTime(opTimeElem.timestamp());
}

void LogicalTime::appendAsOperationTime(BSONObjBuilder* builder) const {
    builder->append(kOperationTimeFieldName, asTimestamp());
}

void LogicalTime::addTicks(uint64_t ticks) {
    _time += ticks;
}

LogicalTime LogicalTime::addTicks(uint64_t ticks) const {
    return LogicalTime(Timestamp(_time + ticks));
}

std::string LogicalTime::toString() const {
    return toBSON().toString();
}

std::array<unsigned char, sizeof(uint64_t)> LogicalTime::toUnsignedArray() const {
    std::array<unsigned char, sizeof(uint64_t)> output;
    DataView(reinterpret_cast<char*>(output.data())).write(LittleEndian<uint64_t>{_time});
    return output;
}

BSONObj LogicalTime::toBSON() const {
    BSONObjBuilder bldr;
    bldr.append("ts", asTimestamp());
    return bldr.obj();
}

void LogicalTime::serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const {
    bob->appendElements(BSON(fieldName << asTimestamp()));
}

LogicalTime LogicalTime::parseFromBSON(const BSONElement& elem) {
    return LogicalTime(elem.timestamp());
}

}  // namespace mongo
