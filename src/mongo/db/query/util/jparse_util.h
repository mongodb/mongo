/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"

namespace mongo {

BSONObj fromFuzzerJson(const char* jsonString, int* len = nullptr);
BSONObj fromFuzzerJson(StringData str);

/**
 * An extended JSON parser that takes in fuzzer output and returns a BSONObj. The underlying
 * functionality primarily uses methods from JParse, while providing custom implementations for
 * keywords with additional support.
 */
class JParseUtil {
public:
    explicit JParseUtil(StringData str);

public:
    // Similar to JParse::object, additionally supporting trailing commas.
    Status object(StringData fieldName, BSONObjBuilder&, bool subObj = true);
    Status parse(BSONObjBuilder& builder);

private:
    Status value(StringData fieldName, BSONObjBuilder&);
    // Similar to JParse::regexObject, additionally supporting regexes inside forward slashes.
    Status regexObject(StringData fieldName, BSONObjBuilder&);
    Status dbRefObject(StringData fieldName, BSONObjBuilder&);
    Status array(StringData fieldName, BSONObjBuilder&, bool subObj = true);
    Status constructor(StringData fieldName, BSONObjBuilder&);
    // Similar to JParse::date, additionally supporting pre-epoch dates and dates in an ISODate
    // format.
    Status date(StringData fieldName, BSONObjBuilder&);
    // Similar to JParse::numberLong, additionally supporting numbers in quotations.
    Status numberLong(StringData fieldName, BSONObjBuilder&);
    Status dbRef(StringData fieldName, BSONObjBuilder&);
    // Similar to JParse::number, but converts all unannotated numerics to doubles.
    Status number(StringData fieldName, BSONObjBuilder&);

    inline bool peekToken(const char* token);
    inline bool readToken(const char* token);

    JParse _jparse;

public:
    inline int offset() const {
        return _jparse.offset();
    }
};

} /* namespace mongo */
