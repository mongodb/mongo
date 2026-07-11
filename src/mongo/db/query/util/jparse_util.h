// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

[[MONGO_MOD_PUBLIC]] BSONObj fromFuzzerJson(const char* jsonString, int* len = nullptr);
[[MONGO_MOD_PUBLIC]] BSONObj fromFuzzerJson(std::string_view str);

/**
 * An extended JSON parser that takes in fuzzer output and returns a BSONObj. The underlying
 * functionality primarily uses methods from JParse, while providing custom implementations for
 * keywords with additional support.
 */
class JParseUtil {
public:
    explicit JParseUtil(std::string_view str);

public:
    // Similar to JParse::object, additionally supporting trailing commas.
    Status object(std::string_view fieldName, BSONObjBuilder&, bool subObj = true);
    Status parse(BSONObjBuilder& builder);

private:
    Status value(std::string_view fieldName, BSONObjBuilder&);
    // Similar to JParse::regexObject, additionally supporting regexes inside forward slashes.
    Status regexObject(std::string_view fieldName, BSONObjBuilder&);
    Status dbRefObject(std::string_view fieldName, BSONObjBuilder&);
    Status array(std::string_view fieldName, BSONObjBuilder&, bool subObj = true);
    Status constructor(std::string_view fieldName, BSONObjBuilder&);
    // Similar to JParse::date, additionally supporting pre-epoch dates and dates in an ISODate
    // format.
    Status date(std::string_view fieldName, BSONObjBuilder&);
    // Similar to JParse::numberLong, additionally supporting numbers in quotations.
    Status numberLong(std::string_view fieldName, BSONObjBuilder&);
    Status dbRef(std::string_view fieldName, BSONObjBuilder&);
    // Similar to JParse::number, but converts all unannotated numerics to doubles.
    Status number(std::string_view fieldName, BSONObjBuilder&);

    inline bool peekToken(const char* token);
    inline bool readToken(const char* token);

    JParse _jparse;

public:
    inline int offset() const {
        return _jparse.offset();
    }
};

} /* namespace mongo */
