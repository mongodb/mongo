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

#include "mongo/db/logical_time.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

const LogicalTime LogicalTime::kUninitialized = LogicalTime();

LogicalTime::LogicalTime(Timestamp ts) : _time(ts.asULL()) {}

LogicalTime LogicalTime::fromOperationTime(const BSONObj& obj) {
    const auto opTimeElem(obj[kOperationTimeFieldName]);
    uassert(ErrorCodes::FailedToParse, "No operationTime found", !opTimeElem.eoo());
    uassert(ErrorCodes::BadValue,
            str::stream() << kOperationTimeFieldName << " is of the wrong type '"
                          << typeName(opTimeElem.type()) << "'",
            opTimeElem.type() == bsonTimestamp);
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

void LogicalTime::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
    bob->appendElements(BSON(fieldName << asTimestamp()));
}

LogicalTime LogicalTime::parseFromBSON(const BSONElement& elem) {
    return LogicalTime(elem.timestamp());
}

}  // namespace mongo
