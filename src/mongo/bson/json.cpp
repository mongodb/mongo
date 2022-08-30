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


#include "mongo/bson/json.h"

#include <algorithm>
#include <cstdint>
#include <fmt/format.h>

#include "mongo/base/parse_number.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/strtoll.h"
#include "mongo/util/base64.h"
#include "mongo/util/ctype.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::ostringstream;
using std::string;
using std::unique_ptr;
using namespace fmt::literals;

#if 0
#define MONGO_JSON_DEBUG(message)                                \
    LOGV2(20107,                                                 \
          "JSON DEBUG @ {FILE_}:{LINE_} {FUNCTION_}{}{message}", \
          "FILE_"_attr = __FILE__,                               \
          "LINE_"_attr = __LINE__,                               \
          "FUNCTION_"_attr = __FUNCTION__,                       \
          ""_attr = ": " \,                                      \
          "message"_attr = message);
#else
#define MONGO_JSON_DEBUG(message)
#endif

#define ALPHA "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
#define DIGIT "0123456789"
#define CONTROL "\a\b\f\n\r\t\v"
#define JOPTIONS "gims"

// Size hints given to char vectors
enum {
    ID_RESERVE_SIZE = 24,
    PAT_RESERVE_SIZE = 4096,
    OPT_RESERVE_SIZE = 64,
    FIELD_RESERVE_SIZE = 4096,
    STRINGVAL_RESERVE_SIZE = 4096,
    BINDATA_RESERVE_SIZE = 4096,
    BINDATATYPE_RESERVE_SIZE = 4096,
    NS_RESERVE_SIZE = 64,
    DB_RESERVE_SIZE = 64,
    NUMBERINT_RESERVE_SIZE = 16,
    NUMBERLONG_RESERVE_SIZE = 20,
    NUMBERDOUBLE_RESERVE_SIZE = 64,
    NUMBERDECIMAL_RESERVE_SIZE = 64,
    DATE_RESERVE_SIZE = 64
};

static const char *LBRACE = "{", *RBRACE = "}", *LBRACKET = "[", *RBRACKET = "]", *LPAREN = "(",
                  *RPAREN = ")", *COLON = ":", *COMMA = ",", *FORWARDSLASH = "/",
                  *SINGLEQUOTE = "'", *DOUBLEQUOTE = "\"";

JParse::JParse(StringData str)
    : _buf(str.rawData()), _input(_buf), _input_end(_input + str.size()) {}

Status JParse::parseError(StringData msg) {
    std::ostringstream ossmsg;
    ossmsg << msg;
    ossmsg << ": offset:";
    ossmsg << offset();
    ossmsg << " of:";
    ossmsg << _buf;
    return Status(ErrorCodes::FailedToParse, ossmsg.str());
}

Status JParse::value(StringData fieldName, BSONObjBuilder& builder) {
    MONGO_JSON_DEBUG("fieldName: " << fieldName);
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
    } else if (readToken("Date")) {
        Status ret = date(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("Timestamp")) {
        Status ret = timestamp(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("ObjectId")) {
        Status ret = objectId(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("NumberLong")) {
        Status ret = numberLong(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("NumberInt")) {
        Status ret = numberInt(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("NumberDecimal")) {
        Status ret = numberDecimal(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (readToken("Dbref") || readToken("DBRef")) {
        Status ret = dbRef(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (peekToken(FORWARDSLASH)) {
        Status ret = regex(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (peekToken(DOUBLEQUOTE) || peekToken(SINGLEQUOTE)) {
        std::string valueString;
        valueString.reserve(STRINGVAL_RESERVE_SIZE);
        Status ret = quotedString(&valueString);
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
            return ret;
        }
    }
    return Status::OK();
}

Status JParse::parse(BSONObjBuilder& builder) {
    return isArray() ? array("UNUSED", builder, false) : object("UNUSED", builder, false);
}

Status JParse::object(StringData fieldName, BSONObjBuilder& builder, bool subObject) {
    MONGO_JSON_DEBUG("fieldName: " << fieldName);
    if (!readToken(LBRACE)) {
        return parseError("Expecting '{'");
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
    Status ret = field(&firstField);
    if (ret != Status::OK()) {
        return ret;
    }

    if (firstField == "$oid") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $oid");
        }
        Status ret = objectIdObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$binary") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $binary");
        }
        Status ret = binaryObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$uuid") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $uuid");
        }
        Status ret = uuidObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$date") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $date");
        }
        Status ret = dateObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$timestamp") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $timestamp");
        }
        Status ret = timestampObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$regex") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $regex");
        }
        Status ret = regexObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$regularExpression") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $regularExpression");
        }
        Status ret = regexObjectCanonical(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$ref") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $ref");
        }
        Status ret = dbRefObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$undefined") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $undefined");
        }
        Status ret = undefinedObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$numberInt") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $numberInt");
        }
        Status ret = numberIntObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$numberLong") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $numberLong");
        }
        Status ret = numberLongObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }

    } else if (firstField == "$numberDouble") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $numberDouble");
        }
        Status ret = numberDoubleObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$numberDecimal") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $numberDecimal");
        }
        Status ret = numberDecimalObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$minKey") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $minKey");
        }
        Status ret = minKeyObject(fieldName, builder);
        if (ret != Status::OK()) {
            return ret;
        }
    } else if (firstField == "$maxKey") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $maxKey");
        }
        Status ret = maxKeyObject(fieldName, builder);
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
            return parseError("Expecting ':'");
        }
        Status valueRet = value(firstField, *objBuilder);
        if (valueRet != Status::OK()) {
            return valueRet;
        }
        while (readToken(COMMA)) {
            std::string fieldName;
            fieldName.reserve(FIELD_RESERVE_SIZE);
            Status fieldRet = field(&fieldName);
            if (fieldRet != Status::OK()) {
                return fieldRet;
            }
            if (!readToken(COLON)) {
                return parseError("Expecting ':'");
            }
            Status valueRet = value(fieldName, *objBuilder);
            if (valueRet != Status::OK()) {
                return valueRet;
            }
        }
    }
    if (!readToken(RBRACE)) {
        return parseError("Expecting '}' or ','");
    }
    return Status::OK();
}

Status JParse::objectIdObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expected ':'");
    }
    std::string id;
    id.reserve(ID_RESERVE_SIZE);
    Status ret = quotedString(&id);
    if (ret != Status::OK()) {
        return ret;
    }
    if (id.size() != 24) {
        return parseError("Expecting 24 hex digits: " + id);
    }
    if (!isHexString(id)) {
        return parseError("Expecting hex digits: " + id);
    }
    builder.append(fieldName, OID(id));
    return Status::OK();
}

Status JParse::binaryObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expected ':'");
    }
    std::string binDataString;
    binDataString.reserve(BINDATA_RESERVE_SIZE);

    std::string binDataType;
    binDataType.reserve(BINDATATYPE_RESERVE_SIZE);

    if (peekToken(LBRACE)) {
        readToken(LBRACE);

        if (!readField("base64")) {
            return parseError("Expected field name: \"base64\", in \"$binary\" object");
        }
        if (!readToken(COLON)) {
            return parseError("Expecting ':'");
        }

        Status dataRet = quotedString(&binDataString);
        if (dataRet != Status::OK()) {
            return dataRet;
        }
        if (!readToken(COMMA)) {
            return parseError("Expected ','");
        }
        if (!readField("subType")) {
            return parseError("Expected field name: \"subType\", in \"$binary\" object");
        }
        if (!readToken(COLON)) {
            return parseError("Expected ':'");
        }
        Status typeRet = quotedString(&binDataType);
        if (typeRet != Status::OK()) {
            return typeRet;
        }
        if (binDataType.size() == 1)
            binDataType = "0" + binDataType;
        readToken(RBRACE);
    } else {
        Status dataRet = quotedString(&binDataString);
        if (dataRet != Status::OK()) {
            return dataRet;
        }
        if (!readToken(COMMA)) {
            return parseError("Expected ','");
        }
        if (!readField("$type")) {
            return parseError("Expected second field name: \"$type\", in \"$binary\" object");
        }
        if (!readToken(COLON)) {
            return parseError("Expected ':'");
        }

        Status typeRet = quotedString(&binDataType);
        if (typeRet != Status::OK()) {
            return typeRet;
        }
    }

    if (binDataString.size() % 4 != 0) {
        return parseError("Invalid length base64 encoded string");
    }
    if (!isBase64String(binDataString)) {
        return parseError("Invalid character in base64 encoded string");
    }
    std::string binData = base64::decode(binDataString);

    if ((binDataType.size() != 2) || !isHexString(binDataType)) {
        return parseError(
            "Argument of $type in $bindata object must be a hex string representation of a "
            "single byte");
    }

    const auto binDataTypeNumeric = hexblob::decodePair(binDataType);

    builder.appendBinData(
        fieldName, binData.length(), BinDataType(binDataTypeNumeric), binData.data());
    return Status::OK();
}

Status JParse::uuidObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expected ':'");
    }
    std::string uuidString;
    uuidString.reserve(40);

    Status dataRet = quotedString(&uuidString);
    if (dataRet != Status::OK()) {
        return dataRet;
    }

    auto uuid = UUID::parse(uuidString);
    if (!uuid.isOK()) {
        return uuid.getStatus();
    }

    uuid.getValue().appendToBuilder(&builder, fieldName);

    return Status::OK();
}

Status JParse::dateObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expected ':'");
    }
    Date_t date;

    if (peekToken(DOUBLEQUOTE)) {
        std::string dateString;
        dateString.reserve(DATE_RESERVE_SIZE);
        Status ret = quotedString(&dateString);
        if (!ret.isOK()) {
            return ret;
        }
        StatusWith<Date_t> dateRet = dateFromISOString(dateString);
        if (!dateRet.isOK()) {
            return dateRet.getStatus();
        }
        date = dateRet.getValue();
    } else if (readToken(LBRACE)) {
        std::string fieldName;
        fieldName.reserve(FIELD_RESERVE_SIZE);
        Status ret = field(&fieldName);
        if (ret != Status::OK()) {
            return ret;
        }
        if (fieldName != "$numberLong") {
            return parseError("Expected field name: $numberLong for $date value object");
        }
        if (!readToken(COLON)) {
            return parseError("Expecting ':'");
        }

        // The number must be a quoted string, since large long numbers could overflow a double
        // and thus may not be valid JSON
        std::string numberLongString;
        numberLongString.reserve(NUMBERLONG_RESERVE_SIZE);
        ret = quotedString(&numberLongString);
        if (!ret.isOK()) {
            return ret;
        }

        long long numberLong;
        ret = NumberParser{}(numberLongString, &numberLong);
        if (!ret.isOK()) {
            return ret;
        }

        readToken(RBRACE);
        date = Date_t::fromMillisSinceEpoch(numberLong);
    } else {
        StatusWith<Date_t> parsedDate = parseDate();
        if (!parsedDate.isOK()) {
            return parsedDate.getStatus();
        }
        date = std::move(parsedDate).getValue();
    }
    builder.appendDate(fieldName, date);
    return Status::OK();
}

Status JParse::timestampObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    if (!readToken(LBRACE)) {
        return parseError("Expecting '{' to start \"$timestamp\" object");
    }

    if (!readField("t")) {
        return parseError("Expected field name \"t\" in \"$timestamp\" sub object");
    }

    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    if (readToken("-")) {
        return parseError("Negative seconds in \"$timestamp\"");
    }
    char* endptr;
    uint32_t seconds;
    NumberParser parser = NumberParser::strToAny(10);
    Status parsedStatus = parser(_input, &seconds, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("Timestamp seconds overflow");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting unsigned integer seconds in \"$timestamp\"");
    }
    _input = endptr;
    if (!readToken(COMMA)) {
        return parseError("Expecting ','");
    }

    if (!readField("i")) {
        return parseError("Expected field name \"i\" in \"$timestamp\" sub object");
    }
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    if (readToken("-")) {
        return parseError("Negative increment in \"$timestamp\"");
    }
    uint32_t count;
    parsedStatus = parser(_input, &count, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("Timestamp increment overflow");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting unsigned integer increment in \"$timestamp\"");
    }
    _input = endptr;

    if (!readToken(RBRACE)) {
        return parseError("Expecting '}'");
    }
    builder.append(fieldName, Timestamp(seconds, count));
    return Status::OK();
}

Status JParse::regexObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    std::string pat;
    pat.reserve(PAT_RESERVE_SIZE);
    Status patRet = quotedString(&pat);
    if (patRet != Status::OK()) {
        return patRet;
    }
    if (readToken(COMMA)) {
        if (!readField("$options")) {
            return parseError("Expected field name: \"$options\" in \"$regex\" object");
        }
        if (!readToken(COLON)) {
            return parseError("Expecting ':'");
        }
        std::string opt;
        opt.reserve(OPT_RESERVE_SIZE);
        Status optRet = quotedString(&opt);
        if (optRet != Status::OK()) {
            return optRet;
        }
        Status optCheckRet = regexOptCheck(opt);
        if (optCheckRet != Status::OK()) {
            return optCheckRet;
        }
        builder.appendRegex(fieldName, pat, opt);
    } else {
        builder.appendRegex(fieldName, pat, "");
    }
    return Status::OK();
}

Status JParse::regexObjectCanonical(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    readToken(LBRACE);
    if (!readField("pattern")) {
        return parseError("Expected field name: \"pattern\", in \"$regularExpression\" object");
    }
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    std::string pat;
    pat.reserve(PAT_RESERVE_SIZE);
    Status patRet = quotedString(&pat);
    if (patRet != Status::OK()) {
        return patRet;
    }
    if (!readToken(COMMA)) {
        return parseError("Expected ','");
    }
    if (!readField("options")) {
        return parseError("Expected field name: \"pattern\", in \"$regularExpression\" object");
    }
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    std::string opt;
    opt.reserve(OPT_RESERVE_SIZE);
    Status optRet = quotedString(&opt);
    if (optRet != Status::OK()) {
        return optRet;
    }
    Status optCheckRet = regexOptCheck(opt);
    if (optCheckRet != Status::OK()) {
        return optCheckRet;
    }
    readToken(RBRACE);
    builder.appendRegex(fieldName, pat, opt);
    return Status::OK();
}

Status JParse::dbRefObject(StringData fieldName, BSONObjBuilder& builder) {
    BSONObjBuilder subBuilder(builder.subobjStart(fieldName));

    if (!readToken(COLON)) {
        return parseError("DBRef: Expecting ':'");
    }
    std::string ns;
    ns.reserve(NS_RESERVE_SIZE);
    Status ret = quotedString(&ns);
    if (ret != Status::OK()) {
        return ret;
    }
    subBuilder.append("$ref", ns);

    if (!readToken(COMMA)) {
        return parseError("DBRef: Expecting ','");
    }

    if (!readField("$id")) {
        return parseError("DBRef: Expected field name: \"$id\" in \"$ref\" object");
    }
    if (!readToken(COLON)) {
        return parseError("DBRef: Expecting ':'");
    }
    Status valueRet = value("$id", subBuilder);
    if (valueRet != Status::OK()) {
        return valueRet;
    }

    if (readToken(COMMA)) {
        if (!readField("$db")) {
            return parseError("DBRef: Expected field name: \"$db\" in \"$ref\" object");
        }
        if (!readToken(COLON)) {
            return parseError("DBRef: Expecting ':'");
        }
        std::string db;
        db.reserve(DB_RESERVE_SIZE);
        ret = quotedString(&db);
        if (ret != Status::OK()) {
            return ret;
        }
        subBuilder.append("$db", db);
    }

    subBuilder.done();
    return Status::OK();
}

Status JParse::undefinedObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    if (!readToken("true")) {
        return parseError("Reserved field \"$undefined\" requires value of true");
    }
    builder.appendUndefined(fieldName);
    return Status::OK();
}

Status JParse::numberLongObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }

    // The number must be a quoted string, since large long numbers could overflow a double and
    // thus may not be valid JSON
    std::string numberLongString;
    numberLongString.reserve(NUMBERLONG_RESERVE_SIZE);
    Status ret = quotedString(&numberLongString);
    if (!ret.isOK()) {
        return ret;
    }

    long long numberLong;
    ret = NumberParser{}(numberLongString, &numberLong);
    if (!ret.isOK()) {
        return ret;
    }

    builder.append(fieldName, numberLong);
    return Status::OK();
}

Status JParse::numberIntObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }

    // The number must be a quoted string, since large long numbers could overflow a double and
    // thus may not be valid JSON
    std::string numberIntString;
    numberIntString.reserve(NUMBERINT_RESERVE_SIZE);
    Status ret = quotedString(&numberIntString);
    if (!ret.isOK()) {
        return ret;
    }

    int numberInt;
    ret = NumberParser{}(numberIntString, &numberInt);
    if (!ret.isOK()) {
        return ret;
    }

    builder.append(fieldName, numberInt);
    return Status::OK();
}

Status JParse::numberDoubleObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    // The number must be a quoted string, since large double numbers could overflow other types
    // and thus may not be valid JSON
    std::string numberDoubleString;
    numberDoubleString.reserve(NUMBERDOUBLE_RESERVE_SIZE);
    Status ret = quotedString(&numberDoubleString);
    if (!ret.isOK()) {
        return ret;
    }

    double numberDouble;
    ret = NumberParser{}(numberDoubleString, &numberDouble);
    if (!ret.isOK()) {
        return ret;
    }

    builder.append(fieldName, numberDouble);
    return Status::OK();
}

Status JParse::numberDecimalObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    // The number must be a quoted string, since large decimal numbers could overflow other types
    // and thus may not be valid JSON
    std::string numberDecimalString;
    numberDecimalString.reserve(NUMBERDECIMAL_RESERVE_SIZE);
    Status ret = quotedString(&numberDecimalString);
    if (!ret.isOK()) {
        return ret;
    }

    Decimal128 numberDecimal(numberDecimalString);

    builder.appendNumber(fieldName, numberDecimal);
    return Status::OK();
}

Status JParse::minKeyObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    if (!readToken("1")) {
        return parseError("Reserved field \"$minKey\" requires value of 1");
    }
    builder.appendMinKey(fieldName);
    return Status::OK();
}

Status JParse::maxKeyObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expecting ':'");
    }
    if (!readToken("1")) {
        return parseError("Reserved field \"$maxKey\" requires value of 1");
    }
    builder.appendMaxKey(fieldName);
    return Status::OK();
}

Status JParse::array(StringData fieldName, BSONObjBuilder& builder, bool subObject) {
    MONGO_JSON_DEBUG("fieldName: " << fieldName);
    if (!readToken(LBRACKET)) {
        return parseError("Expecting '['");
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
        return parseError("Expecting ']' or ','");
    }
    return Status::OK();
}

/* NOTE: this could be easily modified to allow "new" before other
 * constructors, but for now it only allows "new" before Date().
 * Also note that unlike the interactive shell "Date(x)" and "new Date(x)"
 * have the same behavior.  XXX: this may not be desired. */
Status JParse::constructor(StringData fieldName, BSONObjBuilder& builder) {
    if (readToken("Date")) {
        date(fieldName, builder).transitional_ignore();
    } else {
        return parseError("\"new\" keyword not followed by Date constructor");
    }
    return Status::OK();
}

Status JParse::date(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    StatusWith<Date_t> parsedDate = parseDate();
    if (!parsedDate.isOK()) {
        return parsedDate.getStatus();
    }
    Date_t date = parsedDate.getValue();
    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }
    builder.appendDate(fieldName, date);
    return Status::OK();
}

Status JParse::timestamp(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    if (readToken("-")) {
        return parseError("Negative seconds in \"$timestamp\"");
    }
    char* endptr;
    NumberParser parser = NumberParser::strToAny(10);
    uint32_t seconds;
    Status parsedStatus = parser(_input, &seconds, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("Timestamp seconds overflow");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting unsigned integer seconds in \"$timestamp\"");
    }
    _input = endptr;
    if (!readToken(COMMA)) {
        return parseError("Expecting ','");
    }
    if (readToken("-")) {
        return parseError("Negative seconds in \"$timestamp\"");
    }
    uint32_t count;
    parsedStatus = parser(_input, &count, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("Timestamp increment overflow");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting unsigned integer increment in \"$timestamp\"");
    }
    _input = endptr;
    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }
    builder.append(fieldName, Timestamp(seconds, count));
    return Status::OK();
}

Status JParse::objectId(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    std::string id;
    id.reserve(ID_RESERVE_SIZE);
    Status ret = quotedString(&id);
    if (ret != Status::OK()) {
        return ret;
    }
    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }
    if (id.size() != 24) {
        return parseError("Expecting 24 hex digits: " + id);
    }
    if (!isHexString(id)) {
        return parseError("Expecting hex digits: " + id);
    }
    builder.append(fieldName, OID(id));
    return Status::OK();
}

Status JParse::numberLong(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    char* endptr;
    int64_t val;
    Status parsedStatus = NumberParser::strToAny(10)(_input, &val, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("NumberLong out of range");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting number in NumberLong");
    }
    _input = endptr;
    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }
    builder.append(fieldName, static_cast<long long int>(val));
    return Status::OK();
}

Status JParse::numberDecimal(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }

    std::string decString;
    decString.reserve(NUMBERDECIMAL_RESERVE_SIZE);
    Status ret = quotedString(&decString);
    if (ret != Status::OK()) {
        return ret;
    }
    Decimal128 val;
    Status parsedStatus = NumberParser().setDecimal128RoundingMode(
        Decimal128::RoundingMode::kRoundTiesToEven)(decString, &val);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("numberDecimal out of range");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting decimal in numberDecimal");
    }

    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }
    builder.appendNumber(fieldName, val);
    return Status::OK();
}

Status JParse::numberInt(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    char* endptr;
    int32_t val;
    Status parsedStatus = NumberParser::strToAny(10)(_input, &val, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("NumberInt out of range");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Expecting unsigned number in NumberInt");
    }
    _input = endptr;
    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }
    builder.appendNumber(fieldName, static_cast<int>(val));
    return Status::OK();
}

Status JParse::dbRef(StringData fieldName, BSONObjBuilder& builder) {
    BSONObjBuilder subBuilder(builder.subobjStart(fieldName));

    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    std::string ns;
    ns.reserve(NS_RESERVE_SIZE);
    Status refRet = quotedString(&ns);
    if (refRet != Status::OK()) {
        return refRet;
    }
    subBuilder.append("$ref", ns);

    if (!readToken(COMMA)) {
        return parseError("Expecting ','");
    }

    Status valueRet = value("$id", subBuilder);
    if (valueRet != Status::OK()) {
        return valueRet;
    }

    if (readToken(COMMA)) {
        std::string db;
        db.reserve(DB_RESERVE_SIZE);
        Status dbRet = quotedString(&db);
        if (dbRet != Status::OK()) {
            return dbRet;
        }
        subBuilder.append("$db", db);
    }

    if (!readToken(RPAREN)) {
        return parseError("Expecting ')'");
    }

    subBuilder.done();
    return Status::OK();
}

Status JParse::regex(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(FORWARDSLASH)) {
        return parseError("Expecting '/'");
    }
    std::string pat;
    pat.reserve(PAT_RESERVE_SIZE);
    Status patRet = regexPat(&pat);
    if (patRet != Status::OK()) {
        return patRet;
    }
    if (!readToken(FORWARDSLASH)) {
        return parseError("Expecting '/'");
    }
    std::string opt;
    opt.reserve(OPT_RESERVE_SIZE);
    Status optRet = regexOpt(&opt);
    if (optRet != Status::OK()) {
        return optRet;
    }
    Status optCheckRet = regexOptCheck(opt);
    if (optCheckRet != Status::OK()) {
        return optCheckRet;
    }
    builder.appendRegex(fieldName, pat, opt);
    return Status::OK();
}

Status JParse::regexPat(std::string* result) {
    MONGO_JSON_DEBUG("");
    return chars(result, "/");
}

Status JParse::regexOpt(std::string* result) {
    MONGO_JSON_DEBUG("");
    return chars(result, "", JOPTIONS);
}

Status JParse::regexOptCheck(StringData opt) {
    MONGO_JSON_DEBUG("opt: " << opt);
    std::size_t i;
    std::string availableOptions = JOPTIONS;
    for (i = 0; i < opt.size(); i++) {
        std::size_t availIndex = availableOptions.find(opt[i]);
        if (availIndex == std::string::npos) {
            return parseError(string("Bad regex option: ") + opt[i]);
        }
        availableOptions.erase(availIndex, 1);
    }
    return Status::OK();
}

Status JParse::number(StringData fieldName, BSONObjBuilder& builder) {
    char* endptrll;
    char* endptrd;
    long long retll;
    double retd;

    Status parsedStatus = NumberParser::strToAny()(_input, &retd, &endptrd);
    if (parsedStatus == ErrorCodes::Overflow) {
        return parseError("Value cannot fit in double");
    }
    if (!parsedStatus.isOK()) {
        return parseError("Bad characters in value");
    }
    parsedStatus = NumberParser::strToAny(10)(_input, &retll, &endptrll);
    if (endptrll < endptrd || parsedStatus == ErrorCodes::Overflow) {
        // The number either had characters only meaningful for a double or
        // could not fit in a 64 bit int
        MONGO_JSON_DEBUG("Type: double");
        builder.append(fieldName, retd);
    } else if (retll == static_cast<int>(retll)) {
        // The number can fit in a 32 bit int
        MONGO_JSON_DEBUG("Type: 32 bit int");
        builder.append(fieldName, static_cast<int>(retll));
    } else {
        // The number can fit in a 64 bit int
        MONGO_JSON_DEBUG("Type: 64 bit int");
        builder.append(fieldName, retll);
    }
    _input = endptrd;
    if (_input >= _input_end) {
        return parseError("Trailing number at end of input");
    }
    return Status::OK();
}

Status JParse::field(std::string* result) {
    MONGO_JSON_DEBUG("");
    if (peekToken(DOUBLEQUOTE) || peekToken(SINGLEQUOTE)) {
        // Quoted key
        // TODO: make sure quoted field names cannot contain null characters
        return quotedString(result);
    } else {
        // Unquoted key
        while (_input < _input_end && ctype::isSpace(*_input)) {
            ++_input;
        }
        if (_input >= _input_end) {
            return parseError("Field name expected");
        }
        if (!match(*_input, ALPHA "_$")) {
            return parseError("First character in field must be [A-Za-z$_]");
        }
        return chars(result, "", ALPHA DIGIT "_$");
    }
}

Status JParse::quotedString(std::string* result) {
    MONGO_JSON_DEBUG("");
    if (readToken(DOUBLEQUOTE)) {
        Status ret = chars(result, "\"");
        if (ret != Status::OK()) {
            return ret;
        }
        if (!readToken(DOUBLEQUOTE)) {
            return parseError("Expecting '\"'");
        }
    } else if (readToken(SINGLEQUOTE)) {
        Status ret = chars(result, "'");
        if (ret != Status::OK()) {
            return ret;
        }
        if (!readToken(SINGLEQUOTE)) {
            return parseError("Expecting '''");
        }
    } else {
        return parseError("Expecting quoted string");
    }
    return Status::OK();
}

/*
 * terminalSet are characters that signal end of string (e.g.) [ :\0]
 * allowedSet are the characters that are allowed, if this is set
 */
Status JParse::chars(std::string* result, const char* terminalSet, const char* allowedSet) {
    MONGO_JSON_DEBUG("terminalSet: " << terminalSet);
    if (_input >= _input_end) {
        return parseError("Unexpected end of input");
    }
    const char* q = _input;
    while (q < _input_end && !match(*q, terminalSet)) {
        MONGO_JSON_DEBUG("q: " << q);
        if (allowedSet != nullptr) {
            if (!match(*q, allowedSet)) {
                _input = q;
                return Status::OK();
            }
        }
        if (0x00 <= *q && *q <= 0x1F) {
            return parseError("Invalid control character");
        }
        if (*q == '\\' && q + 1 < _input_end) {
            switch (*(++q)) {
                // Escape characters allowed by the JSON spec
                case '"':
                    result->push_back('"');
                    break;
                case '\'':
                    result->push_back('\'');
                    break;
                case '\\':
                    result->push_back('\\');
                    break;
                case '/':
                    result->push_back('/');
                    break;
                case 'b':
                    result->push_back('\b');
                    break;
                case 'f':
                    result->push_back('\f');
                    break;
                case 'n':
                    result->push_back('\n');
                    break;
                case 'r':
                    result->push_back('\r');
                    break;
                case 't':
                    result->push_back('\t');
                    break;
                case 'u': {  // expect 4 hexdigits
                    // TODO: handle UTF-16 surrogate characters
                    ++q;
                    if (q + 4 >= _input_end) {
                        return parseError("Expecting 4 hex digits");
                    }
                    if (!isHexString(StringData(q, 4))) {
                        return parseError("Expecting 4 hex digits");
                    }
                    unsigned char first = hexblob::decodePair(StringData(q, 2));
                    unsigned char second = hexblob::decodePair(StringData(q += 2, 2));
                    const std::string& utf8str = encodeUTF8(first, second);
                    for (unsigned int i = 0; i < utf8str.size(); i++) {
                        result->push_back(utf8str[i]);
                    }
                    ++q;
                    break;
                }
                // Vertical tab character.  Not in JSON spec but allowed in
                // our implementation according to test suite.
                case 'v':
                    result->push_back('\v');
                    break;
                // Escape characters we explicity disallow
                case 'x':
                    return parseError("Hex escape not supported");
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                    return parseError("Octal escape not supported");
                // By default pass on the unescaped character
                default:
                    result->push_back(*q);
                    break;
                    // TODO: check for escaped control characters
            }
            ++q;
        } else {
            result->push_back(*q++);
        }
    }
    if (q < _input_end) {
        _input = q;
        return Status::OK();
    }
    return parseError("Unexpected end of input");
}

std::string JParse::encodeUTF8(unsigned char first, unsigned char second) const {
    std::ostringstream oss;
    if (first == 0 && second < 0x80) {
        oss << second;
    } else if (first < 0x08) {
        oss << char(0xc0 | (first << 2 | second >> 6));
        oss << char(0x80 | (~0xc0 & second));
    } else {
        oss << char(0xe0 | (first >> 4));
        oss << char(0x80 | (~0xc0 & (first << 2 | second >> 6)));
        oss << char(0x80 | (~0xc0 & second));
    }
    return oss.str();
}

inline bool JParse::peekToken(const char* token) {
    return readTokenImpl(token, false);
}

inline bool JParse::readToken(const char* token) {
    return readTokenImpl(token, true);
}

bool JParse::readTokenImpl(const char* token, bool advance) {
    MONGO_JSON_DEBUG("token: " << token);
    const char* check = _input;
    if (token == nullptr) {
        return false;
    }
    while (check < _input_end && ctype::isSpace(*check)) {
        ++check;
    }
    while (*token != '\0') {
        if (check >= _input_end) {
            return false;
        }
        if (*token++ != *check++) {
            return false;
        }
    }
    if (advance) {
        _input = check;
    }
    return true;
}

bool JParse::readField(StringData expectedField) {
    MONGO_JSON_DEBUG("expectedField: " << expectedField);
    std::string nextField;
    nextField.reserve(FIELD_RESERVE_SIZE);
    Status ret = field(&nextField);
    if (ret != Status::OK()) {
        return false;
    }
    if (expectedField != nextField) {
        return false;
    }
    return true;
}

inline bool JParse::match(char matchChar, const char* matchSet) const {
    if (matchSet == nullptr) {
        return true;
    }
    if (*matchSet == '\0') {
        return false;
    }
    return (strchr(matchSet, matchChar) != nullptr);
}

bool JParse::isHexString(StringData str) const {
    MONGO_JSON_DEBUG("str: " << str);
    return std::all_of(str.begin(), str.end(), [](char c) { return ctype::isXdigit(c); });
}

bool JParse::isBase64String(StringData str) const {
    MONGO_JSON_DEBUG("str: " << str);
    return base64::validate(str);
}

bool JParse::isArray() {
    return peekToken(LBRACKET);
}

StatusWith<Date_t> JParse::parseDate() {
    long long msSinceEpoch;
    char* endptr;
    Status parsedStatus = NumberParser::strToAny(10)(_input, &msSinceEpoch, &endptr);
    if (parsedStatus == ErrorCodes::Overflow) {
        /* Need to handle this because jsonString outputs the value of Date_t as unsigned.
         * See SERVER-8330 and SERVER-8573 */
        unsigned long long oldDate;  // Date_t used to be stored as unsigned long longs
        parsedStatus = NumberParser::strToAny(10)(_input, &oldDate, &endptr);
        if (parsedStatus == ErrorCodes::Overflow) {
            return parseError("Date milliseconds overflow");
        }
        msSinceEpoch = static_cast<long long>(oldDate);
    } else if (!parsedStatus.isOK()) {
        return parseError("Date expecting integer milliseconds");
    }
    invariant(endptr != _input);
    Date_t date = Date_t::fromMillisSinceEpoch(msSinceEpoch);
    _input = endptr;
    return date;
}

BSONObj fromjson(const char* jsonString, int* len) {
    MONGO_JSON_DEBUG("jsonString: " << jsonString);
    if (jsonString[0] == '\0') {
        if (len)
            *len = 0;
        return BSONObj();
    }
    JParse jparse(jsonString);
    BSONObjBuilder builder;
    Status ret = Status::OK();
    try {
        ret = jparse.parse(builder);
    } catch (std::exception& e) {
        std::ostringstream message;
        message << "caught exception from within JSON parser: " << e.what();
        uasserted(17031, message.str());
    }

    if (ret != Status::OK()) {
        uasserted(16619, "code {}: {}: {}"_format(ret.code(), ret.codeString(), ret.reason()));
    }
    if (len)
        *len = jparse.offset();
    return builder.obj();
}

BSONObj fromjson(StringData str) {
    return fromjson(str.toString().c_str());
}

std::string tojson(const BSONObj& obj, JsonStringFormat format, bool pretty) {
    return obj.jsonString(format, pretty);
}

std::string tojson(const BSONArray& arr, JsonStringFormat format, bool pretty) {
    return arr.jsonString(format, pretty, true);
}

bool isArray(StringData str) {
    JParse parser(str);
    return parser.isArray();
}

} /* namespace mongo */
