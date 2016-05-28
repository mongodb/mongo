/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/bson/json.h"

#include <cstdint>

#include "mongo/base/parse_number.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/strtoll.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::unique_ptr;
using std::ostringstream;
using std::string;

#if 0
#define MONGO_JSON_DEBUG(message)                                                          \
    log() << "JSON DEBUG @ " << __FILE__ << ":" << __LINE__ << " " << __FUNCTION__ << ": " \
          << message << endl;
#else
#define MONGO_JSON_DEBUG(message)
#endif

#define ALPHA "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
#define DIGIT "0123456789"
#define CONTROL "\a\b\f\n\r\t\v"
#define JOPTIONS "gims"

// Size hints given to char vectors
enum {
    ID_RESERVE_SIZE = 64,
    PAT_RESERVE_SIZE = 4096,
    OPT_RESERVE_SIZE = 64,
    FIELD_RESERVE_SIZE = 4096,
    STRINGVAL_RESERVE_SIZE = 4096,
    BINDATA_RESERVE_SIZE = 4096,
    BINDATATYPE_RESERVE_SIZE = 4096,
    NS_RESERVE_SIZE = 64,
    DB_RESERVE_SIZE = 64,
    NUMBERLONG_RESERVE_SIZE = 64,
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
    } else if (firstField == "$numberLong") {
        if (!subObject) {
            return parseError("Reserved field name in base object: $numberLong");
        }
        Status ret = numberLongObject(fieldName, builder);
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
    Status dataRet = quotedString(&binDataString);
    if (dataRet != Status::OK()) {
        return dataRet;
    }
    if (binDataString.size() % 4 != 0) {
        return parseError("Invalid length base64 encoded string");
    }
    if (!isBase64String(binDataString)) {
        return parseError("Invalid character in base64 encoded string");
    }
    const std::string& binData = base64::decode(binDataString);
    if (!readToken(COMMA)) {
        return parseError("Expected ','");
    }

    if (!readField("$type")) {
        return parseError("Expected second field name: \"$type\", in \"$binary\" object");
    }
    if (!readToken(COLON)) {
        return parseError("Expected ':'");
    }
    std::string binDataType;
    binDataType.reserve(BINDATATYPE_RESERVE_SIZE);
    Status typeRet = quotedString(&binDataType);
    if (typeRet != Status::OK()) {
        return typeRet;
    }
    if ((binDataType.size() != 2) || !isHexString(binDataType)) {
        return parseError(
            "Argument of $type in $bindata object must be a hex string representation of a single "
            "byte");
    }

    // The fromHex function returns a signed char, but the highest
    // BinDataType value is 128, which can only be represented as an
    // unsigned char. If we don't coerce it to an unsigned char before
    // wrapping it in a BinDataType (currently implicitly a signed
    // integer), we get undefined behavior.
    const auto binDataTypeNumeric = static_cast<unsigned char>(fromHex(binDataType));

    builder.appendBinData(
        fieldName, binData.length(), BinDataType(binDataTypeNumeric), binData.data());
    return Status::OK();
}

Status JParse::dateObject(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(COLON)) {
        return parseError("Expected ':'");
    }
    errno = 0;
    char* endptr;
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
        ret = parseNumberFromString(numberLongString, &numberLong);
        if (!ret.isOK()) {
            return ret;
        }
        date = Date_t::fromMillisSinceEpoch(numberLong);
    } else {
        // SERVER-11920: We should use parseNumberFromString here, but that function requires
        // that we know ahead of time where the number ends, which is not currently the case.
        date = Date_t::fromMillisSinceEpoch(strtoll(_input, &endptr, 10));
        if (_input == endptr) {
            return parseError("Date expecting integer milliseconds");
        }
        if (errno == ERANGE) {
            /* Need to handle this because jsonString outputs the value of Date_t as unsigned.
            * See SERVER-8330 and SERVER-8573 */
            errno = 0;
            // SERVER-11920: We should use parseNumberFromString here, but that function
            // requires that we know ahead of time where the number ends, which is not currently
            // the case.
            date =
                Date_t::fromMillisSinceEpoch(static_cast<long long>(strtoull(_input, &endptr, 10)));
            if (errno == ERANGE) {
                return parseError("Date milliseconds overflow");
            }
        }
        _input = endptr;
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
    errno = 0;
    char* endptr;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    uint32_t seconds = strtoul(_input, &endptr, 10);
    if (errno == ERANGE) {
        return parseError("Timestamp seconds overflow");
    }
    if (_input == endptr) {
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
    errno = 0;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    uint32_t count = strtoul(_input, &endptr, 10);
    if (errno == ERANGE) {
        return parseError("Timestamp increment overflow");
    }
    if (_input == endptr) {
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
    ret = parseNumberFromString(numberLongString, &numberLong);
    if (!ret.isOK()) {
        return ret;
    }

    builder.append(fieldName, numberLong);
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
    uint32_t index(0);
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
        do {
            Status ret = value(builder.numStr(index), *arrayBuilder);
            if (ret != Status::OK()) {
                return ret;
            }
            index++;
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
        date(fieldName, builder);
    } else {
        return parseError("\"new\" keyword not followed by Date constructor");
    }
    return Status::OK();
}

Status JParse::date(StringData fieldName, BSONObjBuilder& builder) {
    if (!readToken(LPAREN)) {
        return parseError("Expecting '('");
    }
    errno = 0;
    char* endptr;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    Date_t date = Date_t::fromMillisSinceEpoch(strtoll(_input, &endptr, 10));
    if (_input == endptr) {
        return parseError("Date expecting integer milliseconds");
    }
    if (errno == ERANGE) {
        /* Need to handle this because jsonString outputs the value of Date_t as unsigned.
        * See SERVER-8330 and SERVER-8573 */
        errno = 0;
        // SERVER-11920: We should use parseNumberFromString here, but that function requires
        // that we know ahead of time where the number ends, which is not currently the case.
        date = Date_t::fromMillisSinceEpoch(static_cast<long long>(strtoull(_input, &endptr, 10)));
        if (errno == ERANGE) {
            return parseError("Date milliseconds overflow");
        }
    }
    _input = endptr;
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
    errno = 0;
    char* endptr;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    uint32_t seconds = strtoul(_input, &endptr, 10);
    if (errno == ERANGE) {
        return parseError("Timestamp seconds overflow");
    }
    if (_input == endptr) {
        return parseError("Expecting unsigned integer seconds in \"$timestamp\"");
    }
    _input = endptr;
    if (!readToken(COMMA)) {
        return parseError("Expecting ','");
    }
    if (readToken("-")) {
        return parseError("Negative seconds in \"$timestamp\"");
    }
    errno = 0;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    uint32_t count = strtoul(_input, &endptr, 10);
    if (errno == ERANGE) {
        return parseError("Timestamp increment overflow");
    }
    if (_input == endptr) {
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
    errno = 0;
    char* endptr;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    int64_t val = strtoll(_input, &endptr, 10);
    if (errno == ERANGE) {
        return parseError("NumberLong out of range");
    }
    if (_input == endptr) {
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
    Decimal128 val(decString);

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
    errno = 0;
    char* endptr;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    int32_t val = strtol(_input, &endptr, 10);
    if (errno == ERANGE) {
        return parseError("NumberInt out of range");
    }
    if (_input == endptr) {
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
    for (i = 0; i < opt.size(); i++) {
        if (!match(opt[i], JOPTIONS)) {
            return parseError(string("Bad regex option: ") + opt[i]);
        }
    }
    return Status::OK();
}

Status JParse::number(StringData fieldName, BSONObjBuilder& builder) {
    char* endptrll;
    char* endptrd;
    long long retll;
    double retd;

    // reset errno to make sure that we are getting it from strtod
    errno = 0;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    retd = strtod(_input, &endptrd);
    // if pointer does not move, we found no digits
    if (_input == endptrd) {
        return parseError("Bad characters in value");
    }
    if (errno == ERANGE) {
        return parseError("Value cannot fit in double");
    }
    // reset errno to make sure that we are getting it from strtoll
    errno = 0;
    // SERVER-11920: We should use parseNumberFromString here, but that function requires that
    // we know ahead of time where the number ends, which is not currently the case.
    retll = strtoll(_input, &endptrll, 10);
    if (endptrll < endptrd || errno == ERANGE) {
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
        // 'isspace()' takes an 'int' (signed), so (default signed) 'char's get sign-extended
        // and therefore 'corrupted' unless we force them to be unsigned ... 0x80 becomes
        // 0xffffff80 as seen by isspace when sign-extended ... we want it to be 0x00000080
        while (_input < _input_end && isspace(*reinterpret_cast<const unsigned char*>(_input))) {
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
        if (allowedSet != NULL) {
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
                    unsigned char first = fromHex(q);
                    unsigned char second = fromHex(q += 2);
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
    if (token == NULL) {
        return false;
    }
    // 'isspace()' takes an 'int' (signed), so (default signed) 'char's get sign-extended
    // and therefore 'corrupted' unless we force them to be unsigned ... 0x80 becomes
    // 0xffffff80 as seen by isspace when sign-extended ... we want it to be 0x00000080
    while (check < _input_end && isspace(*reinterpret_cast<const unsigned char*>(check))) {
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
    if (matchSet == NULL) {
        return true;
    }
    if (*matchSet == '\0') {
        return false;
    }
    return (strchr(matchSet, matchChar) != NULL);
}

bool JParse::isHexString(StringData str) const {
    MONGO_JSON_DEBUG("str: " << str);
    std::size_t i;
    for (i = 0; i < str.size(); i++) {
        if (!isxdigit(str[i])) {
            return false;
        }
    }
    return true;
}

bool JParse::isBase64String(StringData str) const {
    MONGO_JSON_DEBUG("str: " << str);
    std::size_t i;
    for (i = 0; i < str.size(); i++) {
        if (!match(str[i], base64::chars)) {
            return false;
        }
    }
    return true;
}

bool JParse::isArray() {
    return peekToken(LBRACKET);
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
        throw MsgAssertionException(17031, message.str());
    }

    if (ret != Status::OK()) {
        ostringstream message;
        message << "code " << ret.code() << ": " << ret.codeString() << ": " << ret.reason();
        throw MsgAssertionException(16619, message.str());
    }
    if (len)
        *len = jparse.offset();
    return builder.obj();
}

BSONObj fromjson(const std::string& str) {
    return fromjson(str.c_str());
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
