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
#include "mongo/db/query/util/jparse_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/datetime/date_time_support.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/lexical_cast.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using std::ostringstream;
using std::string;
using std::unique_ptr;

#if 0
#define MONGO_JPARSE_UTIL_DEBUG(message)                                \
    LOGV2(9180300,                                                      \
          "JPARSE UTIL DEBUG @ {FILE_}:{LINE_} {FUNCTION_}{}{message}", \
          "FILE_"_attr = __FILE__,                                      \
          "LINE_"_attr = __LINE__,                                      \
          "FUNCTION_"_attr = __FUNCTION__,                              \
          ""_attr = ": " \,                                             \
          "message"_attr = message);
#else
#define MONGO_JPARSE_UTIL_DEBUG(message)
#endif

namespace {
// Size hints given to char vectors
enum {
    PAT_RESERVE_SIZE = 4096,
    OPT_RESERVE_SIZE = 64,
    FIELD_RESERVE_SIZE = 4096,
    STRINGVAL_RESERVE_SIZE = 4096,
    NS_RESERVE_SIZE = 64,
    DB_RESERVE_SIZE = 64,
    DATE_RESERVE_SIZE = 64
};

static const char *LBRACE = "{", *RBRACE = "}", *LBRACKET = "[", *RBRACKET = "]", *LPAREN = "(",
                  *RPAREN = ")", *COLON = ":", *COMMA = ",", *FORWARDSLASH = "/",
                  *SINGLEQUOTE = "'", *DOUBLEQUOTE = "\"";
}  // namespace

JParseUtil::JParseUtil(StringData str) : _jparse(str) {}

Status JParseUtil::value(StringData fieldName, BSONObjBuilder& builder) {
    MONGO_JPARSE_UTIL_DEBUG("fieldName: " << fieldName);
    if (peekToken(LBRACE)) {
        Status ret = object(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (peekToken(LBRACKET)) {
        Status ret = array(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("new")) {
        Status ret = constructor(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("Date") || readToken("ISODate")) {
        Status ret = date(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("Timestamp")) {
        Status ret = _jparse.timestamp(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("ObjectId")) {
        Status ret = _jparse.objectId(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("NumberLong")) {
        Status ret = numberLong(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("NumberInt")) {
        Status ret = _jparse.numberInt(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("NumberDecimal")) {
        Status ret = _jparse.numberDecimal(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("Dbref") || readToken("DBRef")) {
        Status ret = dbRef(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("UUID")) {
        Status ret = _jparse.uuid(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (peekToken(FORWARDSLASH)) {
        Status ret = _jparse.regex(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (peekToken(DOUBLEQUOTE) || peekToken(SINGLEQUOTE)) {
        std::string valueString;
        valueString.reserve(STRINGVAL_RESERVE_SIZE);
        Status ret = _jparse.quotedString(&valueString);
        if (ret != Status::OK()) {
            return ret;
        }
        builder.append(fieldName, valueString);
    } else if (readToken("true")) {
        builder.append(fieldName, true);
    } else if (readToken("false")) {
        builder.append(fieldName, false);
    } else if (readToken("null")) {
        builder.appendNull(fieldName);
    } else if (readToken("undefined")) {
        builder.appendUndefined(fieldName);
    } else if (readToken("NaN")) {
        builder.append(fieldName, std::numeric_limits<double>::quiet_NaN());
    } else if (readToken("Infinity")) {
        builder.append(fieldName, std::numeric_limits<double>::infinity());
    } else if (readToken("-Infinity")) {
        builder.append(fieldName, -std::numeric_limits<double>::infinity());
    } else {
        Status ret = number(fieldName, builder);
        if (ret != Status::OK()) {
            return ret.withContext(
                "Attempted to parse a number array element, not recognizing any other keywords. "
                "Perhaps you left a trailing comma or forgot a '{'?");
        }
    }
    return Status::OK();
}

Status JParseUtil::parse(BSONObjBuilder& builder) {
    return _jparse.isArray() ? array("UNUSED", builder, false) : object("UNUSED", builder, false);
}

Status JParseUtil::object(StringData fieldName, BSONObjBuilder& builder, bool subObject) {
    MONGO_JPARSE_UTIL_DEBUG("fieldName: " << fieldName);
    if (!readToken(LBRACE)) {
        return _jparse.parseError("Expecting '{'");
    }

    // Empty object
    if (readToken(RBRACE)) {
        if (subObject) {
            BSONObjBuilder empty(builder.subobjStart(fieldName));
            empty.done();
        }
        return Status::OK();
    }

    // Special object
    std::string firstField;
    firstField.reserve(FIELD_RESERVE_SIZE);
    Status fieldParseResult = _jparse.field(&firstField);
    if (fieldParseResult != Status::OK()) {
        return fieldParseResult;
    }

    if (firstField == "$oid") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $oid");
        }
        Status ret = _jparse.objectIdObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$binary") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $binary");
        }
        Status ret = _jparse.binaryObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$uuid") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $uuid");
        }
        Status ret = _jparse.uuidObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$date") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $date");
        }
        Status ret = _jparse.dateObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$timestamp") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $timestamp");
        }
        Status ret = _jparse.timestampObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$regex") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $regex");
        }
        Status ret = regexObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$regularExpression") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $regularExpression");
        }
        Status ret = _jparse.regexObjectCanonical(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$ref") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $ref");
        }
        Status ret = dbRefObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$undefined") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $undefined");
        }
        Status ret = _jparse.undefinedObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$numberInt") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $numberInt");
        }
        Status ret = _jparse.numberIntObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$numberLong") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $numberLong");
        }
        Status ret = _jparse.numberLongObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }

    } else if (firstField == "$numberDouble") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $numberDouble");
        }
        Status ret = _jparse.numberDoubleObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$numberDecimal") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $numberDecimal");
        }
        Status ret = _jparse.numberDecimalObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$minKey") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $minKey");
        }
        Status ret = _jparse.minKeyObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$maxKey") {
        if (!subObject) {
            return _jparse.parseError("Reserved field name in base object: $maxKey");
        }
        Status ret = _jparse.maxKeyObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else {  // firstField != <reserved field name>
        // Normal object

        // Only create a sub builder if this is not the base object
        BSONObjBuilder* objBuilder = &builder;
        unique_ptr<BSONObjBuilder> subObjBuilder;
        if (subObject) {
            subObjBuilder.reset(new BSONObjBuilder(builder.subobjStart(fieldName)));
            objBuilder = subObjBuilder.get();
        }

        if (!readToken(COLON)) {
            return _jparse.parseError("Expecting ':'");
        }
        Status valueRet = value(firstField, *objBuilder);
        if (valueRet != Status::OK()) {
            return valueRet;
        }
        while (readToken(COMMA)) {
            // Allow trailing commas followed by a closing brace.
            if (peekToken(RBRACE)) {
                break;
            }
            std::string nextFieldName;
            nextFieldName.reserve(FIELD_RESERVE_SIZE);
            Status fieldRet = _jparse.field(&nextFieldName);
            if (fieldRet != Status::OK()) {
                return fieldRet;
            }
            if (!readToken(COLON)) {
                return _jparse.parseError("Expecting ':'");
            }
            Status nextFieldValueRet = value(nextFieldName, *objBuilder);
            if (nextFieldValueRet != Status::OK()) {
                return nextFieldValueRet;
            }
        }
    }
    if (!readToken(RBRACE)) {
        return _jparse.parseError("Expecting '}' or ','");
    }
    return Status::OK();
}

Status JParseUtil::regexObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return _jparse.parseError("Expecting ':'");
    }
    std::string pat;
    pat.reserve(PAT_RESERVE_SIZE);

    // Allow regex objects beginning with a forward slash, as well as those in quotations.
    if (peekToken(FORWARDSLASH)) {
        if (!readToken(FORWARDSLASH)) {
            return _jparse.parseError("Expecting '/'");
        }
        Status patRet = _jparse.regexPat(&pat);
        if (patRet != Status::OK()) {
            return patRet;
        }
        if (!readToken(FORWARDSLASH)) {
            return _jparse.parseError("Expecting '/'");
        }
    } else {
        Status patRet = _jparse.quotedString(&pat);
        if (patRet != Status::OK()) {
            return patRet;
        }
    }

    if (readToken(COMMA)) {
        if (!_jparse.readField("$options")) {
            return _jparse.parseError("Expected field name: \"$options\" in \"$regex\" object");
        }
        if (!readToken(COLON)) {
            return _jparse.parseError("Expecting ':'");
        }
        std::string opt;
        opt.reserve(OPT_RESERVE_SIZE);
        Status optRet = _jparse.quotedString(&opt);
        if (optRet != Status::OK()) {
            return optRet;
        }
        Status optCheckRet = _jparse.regexOptCheck(opt);
        if (optCheckRet != Status::OK()) {
            return optCheckRet;
        }
        builder.appendRegex(fieldName, pat, opt);
    } else {
        builder.appendRegex(fieldName, pat, "");
    }
    return Status::OK();
}

Status JParseUtil::dbRefObject(StringData fieldName, BSONObjBuilder& builder) {
    BSONObjBuilder subBuilder(builder.subobjStart(fieldName));

    if (!readToken(COLON)) {
        return _jparse.parseError("DBRef: Expecting ':'");
    }
    std::string ns;
    ns.reserve(NS_RESERVE_SIZE);
    Status ret = _jparse.quotedString(&ns);
    if (ret != Status::OK()) {
        return ret;
    }
    subBuilder.append("$ref", ns);

    if (!readToken(COMMA)) {
        return _jparse.parseError("DBRef: Expecting ','");
    }

    if (!_jparse.readField("$id")) {
        return _jparse.parseError("DBRef: Expected field name: \"$id\" in \"$ref\" object");
    }
    if (!readToken(COLON)) {
        return _jparse.parseError("DBRef: Expecting ':'");
    }
    Status valueRet = value("$id", subBuilder);
    if (valueRet != Status::OK()) {
        return valueRet;
    }

    if (readToken(COMMA)) {
        if (!_jparse.readField("$db")) {
            return _jparse.parseError("DBRef: Expected field name: \"$db\" in \"$ref\" object");
        }
        if (!readToken(COLON)) {
            return _jparse.parseError("DBRef: Expecting ':'");
        }
        std::string db;
        db.reserve(DB_RESERVE_SIZE);
        ret = _jparse.quotedString(&db);
        if (ret != Status::OK()) {
            return ret;
        }
        subBuilder.append("$db", db);
    }

    subBuilder.done();
    return Status::OK();
}

Status JParseUtil::array(StringData fieldName, BSONObjBuilder& builder, bool subObject) {
    MONGO_JPARSE_UTIL_DEBUG("fieldName: " << fieldName);
    if (!readToken(LBRACKET)) {
        return _jparse.parseError("Expecting '['");
    }

    BSONObjBuilder* arrayBuilder = &builder;
    unique_ptr<BSONObjBuilder> subObjBuilder;
    if (subObject) {
        subObjBuilder.reset(new BSONObjBuilder(builder.subarrayStart(fieldName)));
        arrayBuilder = subObjBuilder.get();
    }

    if (!peekToken(RBRACKET)) {
        DecimalCounter<uint32_t> index;
        do {
            Status ret = value(StringData{index}, *arrayBuilder);
            if (!ret.isOK()) {
                return ret;
            }
            ++index;
        } while (readToken(COMMA));
    }
    arrayBuilder->done();
    if (!readToken(RBRACKET)) {
        return _jparse.parseError("Expecting ']' or ','");
    }
    return Status::OK();
}

/* NOTE: this could be easily modified to allow "new" before other
 * constructors, but for now it only allows "new" before Date().
 * Also note that unlike the interactive shell "Date(x)" and "new Date(x)"
 * have the same behavior.  XXX: this may not be desired. */
Status JParseUtil::constructor(StringData fieldName, BSONObjBuilder& builder) {
    if (readToken("Date")) {
        date(fieldName, builder).transitional_ignore();
    } else {
        return _jparse.parseError("\"new\" keyword not followed by Date constructor");
    }
    return Status::OK();
}

Status JParseUtil::date(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return _jparse.parseError("Expecting '('");
    }

    Date_t date;
    if (peekToken(DOUBLEQUOTE) || peekToken(SINGLEQUOTE)) {
        std::string dateString;
        dateString.reserve(DATE_RESERVE_SIZE);
        Status ret = _jparse.quotedString(&dateString);
        if (!ret.isOK()) {
            return ret;
        }

        // Support pre-epoch dates with the date_time_support library.
        if (boost::lexical_cast<int>(dateString.substr(0, 4)) < 1970) {
            const TimeZoneDatabase kDefaultTimeZoneDatabase{};
            const TimeZone kDefaultTimeZone = TimeZoneDatabase::utcZone();
            auto format = "%Y-%m-%dT%H:%M:%SZ"_sd;
            date = kDefaultTimeZoneDatabase.fromString(dateString, kDefaultTimeZone, format);
        } else {
            // Parse date strings in an ISODate format.
            StatusWith<Date_t> dateRet = dateFromISOString(dateString);
            if (!dateRet.isOK()) {
                return dateRet.getStatus();
            }
            date = dateRet.getValue();
        }
    } else {
        StatusWith<Date_t> parsedDate = _jparse.parseDate();
        if (!parsedDate.isOK()) {
            return parsedDate.getStatus();
        }
        date = parsedDate.getValue();
    }

    if (!readToken(RPAREN)) {
        return _jparse.parseError("Expecting ')'");
    }
    builder.appendDate(fieldName, date);
    return Status::OK();
}

Status JParseUtil::numberLong(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return _jparse.parseError("Expecting '('");
    }
    char* endptr;
    int64_t val;

    // Check if the number begins in quotations.
    auto quotedNum = peekToken(DOUBLEQUOTE);
    if (quotedNum) {
        readToken(DOUBLEQUOTE);
    }

    Status parsedStatus = NumberParser::strToAny(10)(_jparse._input, &val, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return _jparse.parseError("NumberLong out of range");
    }
    if (!parsedStatus.isOK()) {
        return _jparse.parseError("Expecting number in NumberLong");
    }
    _jparse._input.remove_prefix(endptr - _jparse._input.data());

    // Check that a number beginning in quotations ends in quotations.
    if (quotedNum && !readToken(DOUBLEQUOTE)) {
        return _jparse.parseError("Expecting \"");
    }

    if (!readToken(RPAREN)) {
        return _jparse.parseError("Expecting ')'");
    }
    builder.append(fieldName, static_cast<long long int>(val));
    return Status::OK();
}

Status JParseUtil::dbRef(StringData fieldName, BSONObjBuilder& builder) {
    BSONObjBuilder subBuilder(builder.subobjStart(fieldName));

    if (!readToken(LPAREN)) {
        return _jparse.parseError("Expecting '('");
    }
    std::string ns;
    ns.reserve(NS_RESERVE_SIZE);
    Status refRet = _jparse.quotedString(&ns);
    if (refRet != Status::OK()) {
        return refRet;
    }
    subBuilder.append("$ref", ns);

    if (!readToken(COMMA)) {
        return _jparse.parseError("Expecting ','");
    }

    Status valueRet = value("$id", subBuilder);
    if (valueRet != Status::OK()) {
        return valueRet;
    }

    if (readToken(COMMA)) {
        std::string db;
        db.reserve(DB_RESERVE_SIZE);
        Status dbRet = _jparse.quotedString(&db);
        if (dbRet != Status::OK()) {
            return dbRet;
        }
        subBuilder.append("$db", db);
    }

    if (!readToken(RPAREN)) {
        return _jparse.parseError("Expecting ')'");
    }

    subBuilder.done();
    return Status::OK();
}

Status JParseUtil::number(StringData fieldName, BSONObjBuilder& builder) {
    char* endptrd;
    double retd;

    Status parsedStatus = NumberParser::strToAny()(_jparse._input, &retd, &endptrd);
    if (parsedStatus == ErrorCodes::Overflow) {
        return _jparse.parseError("Value cannot fit in double");
    }
    if (!parsedStatus.isOK()) {
        return _jparse.parseError(parsedStatus.withContext("Bad characters in value").reason());
    }

    // Always cast the parsed number to a double.
    MONGO_JPARSE_UTIL_DEBUG("Type: double");
    builder.append(fieldName, retd);

    _jparse._input.remove_prefix(endptrd - _jparse._input.data());
    if (_jparse._input.empty()) {
        return _jparse.parseError("Trailing number at end of input");
    }
    return Status::OK();
}

inline bool JParseUtil::peekToken(const char* token) {
    return _jparse.readTokenImpl(token, false);
}

inline bool JParseUtil::readToken(const char* token) {
    return _jparse.readTokenImpl(token, true);
}

BSONObj fromFuzzerJson(const char* jsonString, int* len) {
    MONGO_JPARSE_UTIL_DEBUG("jsonString: " << jsonString);
    if (jsonString[0] == '\0') {
        if (len)
            *len = 0;
        return BSONObj();
    }
    JParseUtil jparseutil(jsonString);
    BSONObjBuilder builder;
    Status ret = Status::OK();
    try {
        ret = jparseutil.parse(builder);
    } catch (std::exception& e) {
        std::ostringstream message;
        message << "caught exception from within JSON parser: " << e.what();
        uasserted(9180301, message.str());
    }

    if (ret != Status::OK()) {
        uasserted(
            9180302,
            fmt::format(
                "code {}: {}: {}", fmt::underlying(ret.code()), ret.codeString(), ret.reason()));
    }
    if (len)
        *len = jparseutil.offset();
    return builder.obj();
}

BSONObj fromFuzzerJson(StringData str) {
    return fromFuzzerJson(std::string{str}.c_str());
}

} /* namespace mongo */
