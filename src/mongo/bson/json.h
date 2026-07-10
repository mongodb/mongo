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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <iosfwd>
#include <string>
#include <string_view>

namespace mongo {

/**
 * Create a BSONObj from a JSON <http://www.json.org>,
 * <http://www.ietf.org/rfc/rfc4627.txt> string.  In addition to the JSON
 * extensions extensions described here
 * <http://dochub.mongodb.org/core/mongodbextendedjson>, this function
 * accepts unquoted field names and allows single quotes to optionally be
 * used when specifying field names and std::string values instead of double
 * quotes.  JSON unicode escape sequences (of the form \uXXXX) are
 * converted to utf8.
 * `str` must be a null terminated string.
 *
 * `str` must be completely consumed by the parse.
 * Trailing whitespace is tolerated.
 *
 * Throws with `ErrorCodes::FailedToParse` on failure.
 *   - If the parse failed because the `str` was incompletely consumed,
 *     the exception message will indicate "Garbage at end".
 *   - Otherwise, the message includes the character offset where parsing failed.
 */


[[MONGO_MOD_PUBLIC]] BSONObj fromjson(std::string_view str);

/**
 * Convert a BSONArray to a JSON string.
 *
 * @param arr The BSON Array.
 * @param format The JSON format (TenGen, Strict).
 * @param pretty Enables pretty output.
 */
[[MONGO_MOD_PUBLIC]] std::string tojson(const BSONArray& arr,
                                        JsonStringFormat format = ExtendedCanonicalV2_0_0,
                                        bool pretty = false);

/**
 * Convert a BSONObj to a JSON string.
 *
 * @param obj The BSON Object.
 * @param format The JSON format (JS, TenGen, Strict).
 * @param pretty Enables pretty output.
 */
[[MONGO_MOD_PUBLIC]] std::string tojson(const BSONObj& obj,
                                        JsonStringFormat format = ExtendedCanonicalV2_0_0,
                                        bool pretty = false);

class JParseUtil;

/**
 * Parser class.  A BSONObj is constructed incrementally by passing a
 * BSONObjBuilder to the recursive parsing methods.  The grammar for the
 * element parsed is described before each function.
 */
class JParse {
    friend class JParseUtil;

public:
    constexpr static int kMaxDepth = BSONDepth::kDefaultMaxAllowableDepth;
    explicit JParse(std::string_view str) : _buf(str), _input(str) {}

    /*
     * Notation: All-uppercase symbols denote non-terminals; all other
     * symbols are literals.
     */

    /*
     * VALUE :
     *     STRING
     *   | NUMBER
     *   | NUMBERINT
     *   | NUMBERLONG
     *   | NUMBERDECIMAL
     *   | OBJECT
     *   | ARRAY
     *
     *   | true
     *   | false
     *   | null
     *   | undefined
     *
     *   | NaN
     *   | Infinity
     *   | -Infinity
     *
     *   | DATE
     *   | TIMESTAMP
     *   | REGEX
     *   | OBJECTID
     *   | DBREF
     *
     *   | new CONSTRUCTOR
     */

private:
    Status value(std::string_view fieldName, BSONObjBuilder&, int depth);

    /*
     * OBJECT :
     *     {}
     *   | { MEMBERS }
     *   | SPECIALOBJECT
     *
     * MEMBERS :
     *     PAIR
     *   | PAIR , MEMBERS
     *
     * PAIR :
     *     FIELD : VALUE
     *
     * SPECIALOBJECT :
     *     OIDOBJECT
     *   | BINARYOBJECT
     *   | DATEOBJECT
     *   | TIMESTAMPOBJECT
     *   | REGEXOBJECT
     *   | REFOBJECT
     *   | UNDEFINEDOBJECT
     *   | NUMBERLONGOBJECT
     *   | NUMBERDECIMALOBJECT
     *   | MINKEYOBJECT
     *   | MAXKEYOBJECT
     *
     */
public:
    Status object(std::string_view fieldName, BSONObjBuilder&, bool subObj = true, int depth = 0);
    Status parse(BSONObjBuilder& builder);
    bool isArray();

private:
    /* The following functions are called with the '{' and the first
     * field already parsed since they are both implied given the
     * context. */
    /*
     * OIDOBJECT :
     *     { FIELD("$oid") : <24 character hex std::string> }
     */
    Status objectIdObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * BINARYOBJECT :
     *     { FIELD("$binary") : <base64 representation of a binary std::string>,
     *          FIELD("$type") : <hexadecimal representation of a single byte
     *              indicating the data type> }
     */
    Status binaryObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * UUIDOBJECT :
     *     { FIELD("$uuid") : <string representation of UUID, in hexadecimal per RFC 4122> }
     */
    Status uuidObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * DATEOBJECT :
     *     { FIELD("$date") : <64 bit signed integer for milliseconds since epoch> }
     */
    Status dateObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * TIMESTAMPOBJECT :
     *     { FIELD("$timestamp") : {
     *         FIELD("t") : <32 bit unsigned integer for seconds since epoch>,
     *         FIELD("i") : <32 bit unsigned integer for the increment> } }
     */
    Status timestampObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     *     NOTE: the rules for the body of the regex are different here,
     *     since it is quoted instead of surrounded by slashes.
     * REGEXOBJECT :
     *     { FIELD("$regex") : <string representing body of regex> }
     *   | { FIELD("$regex") : <string representing body of regex>,
     *          FIELD("$options") : <string representing regex options> }
     */
    Status regexObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     *     NOTE: the rules for the body of the regex are different here,
     *     since it is quoted instead of surrounded by slashes.
     * REGEXOBJECT :
     *     { FIELD("$regularExpression") : {
     *         FIELD("pattern") : <string representing body of regex>,
     *         FIELD("options") : <string representing regex options> } }
     */
    Status regexObjectCanonical(std::string_view fieldName, BSONObjBuilder&);

    /*
     * REFOBJECT :
     *     { FIELD("$ref") : <string representing collection name>,
     *          FIELD("$id") : <24 character hex std::string> }
     *   | { FIELD("$ref") : std::string , FIELD("$id") : OBJECTID }
     *   | { FIELD("$ref") : std::string , FIELD("$id") : OIDOBJECT }
     */
    Status dbRefObject(std::string_view fieldName, BSONObjBuilder&, int depth);

    /*
     * UNDEFINEDOBJECT :
     *     { FIELD("$undefined") : true }
     */
    Status undefinedObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * NUMBERINTOBJECT :
     *     { FIELD("$numberInt") : "<number>" }
     */
    Status numberIntObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * NUMBERLONGOBJECT :
     *     { FIELD("$numberLong") : "<number>" }
     */
    Status numberLongObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * NUMBERDOUBLEOBJECT :
     *     { FIELD("$numberDouble") : "<number>" }
     */
    Status numberDoubleObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * NUMBERDECIMALOBJECT :
     *     { FIELD("$numberDecimal") : "<number>" }
     */
    Status numberDecimalObject(std::string_view fieldName, BSONObjBuilder&);

    /*
     * MINKEYOBJECT :
     *     { FIELD("$minKey") : 1 }
     */
    Status minKeyObject(std::string_view fieldName, BSONObjBuilder& builder);

    /*
     * MAXKEYOBJECT :
     *     { FIELD("$maxKey") : 1 }
     */
    Status maxKeyObject(std::string_view fieldName, BSONObjBuilder& builder);

    /*
     * ARRAY :
     *     []
     *   | [ ELEMENTS ]
     *
     * ELEMENTS :
     *     VALUE
     *   | VALUE , ELEMENTS
     */
    Status array(std::string_view fieldName, BSONObjBuilder&, bool subObj, int depth);

    /*
     * NOTE: Currently only Date can be preceded by the "new" keyword
     * CONSTRUCTOR :
     *     DATE
     */
    Status constructor(std::string_view fieldName, BSONObjBuilder&);

    /* The following functions only parse the body of the constructor
     * between the parentheses, not including the constructor name */
    /*
     * DATE :
     *     Date( <64 bit signed integer for milliseconds since epoch> )
     */
    Status date(std::string_view fieldName, BSONObjBuilder&);

    /*
     * TIMESTAMP :
     *     Timestamp( <32 bit unsigned integer for seconds since epoch>,
     *          <32 bit unsigned integer for the increment> )
     */
    Status timestamp(std::string_view fieldName, BSONObjBuilder&);

    /*
     * OBJECTID :
     *     ObjectId( <24 character hex std::string> )
     */
    Status objectId(std::string_view fieldName, BSONObjBuilder&);

    /*
     * UUID :
     *     UUID( <36 character [<hex>, '-'] std::string> )
     */
    Status uuid(std::string_view fieldName, BSONObjBuilder& builder);

    /*
     * NUMBERLONG :
     *     NumberLong( <number> )
     */
    Status numberLong(std::string_view fieldName, BSONObjBuilder&);

    /*
     * NUMBERDECIMAL :
     *     NumberDecimal( <number> )
     */
    Status numberDecimal(std::string_view fieldName, BSONObjBuilder&);

    /*
     * NUMBERINT :
     *     NumberInt( <number> )
     */
    Status numberInt(std::string_view fieldName, BSONObjBuilder&);

    /*
     * DBREF :
     *     Dbref( <namespace std::string> , <24 character hex std::string> )
     */
    Status dbRef(std::string_view fieldName, BSONObjBuilder&, int depth);

    /*
     * REGEX :
     *     / REGEXCHARS / REGEXOPTIONS
     *
     * REGEXCHARS :
     *     REGEXCHAR
     *   | REGEXCHAR REGEXCHARS
     *
     * REGEXCHAR :
     *     any-Unicode-character-except-/-or-\-or-CONTROLCHAR
     *   | \"
     *   | \'
     *   | \\
     *   | \/
     *   | \b
     *   | \f
     *   | \n
     *   | \r
     *   | \t
     *   | \v
     *   | \u HEXDIGIT HEXDIGIT HEXDIGIT HEXDIGIT
     *   | \any-Unicode-character-except-x-or-[0-7]
     *
     * REGEXOPTIONS :
     *     REGEXOPTION
     *   | REGEXOPTION REGEXOPTIONS
     *
     * REGEXOPTION :
     *     g | i | m | s
     */
    Status regex(std::string_view fieldName, BSONObjBuilder&);
    Status regexPat(std::string* result);
    Status regexOpt(std::string* result);
    Status regexOptCheck(std::string_view opt);

    /*
     * NUMBER :
     *
     * NOTE: Number parsing is based on standard library functions, not
     * necessarily on the JSON numeric grammar.
     *
     * Number as value - strtoll and strtod
     * Date - strtoll
     * Timestamp - strtoul for both timestamp and increment and '-'
     * before a number explicity disallowed
     */
    Status number(std::string_view fieldName, BSONObjBuilder&);

    /*
     * FIELD :
     *     STRING
     *   | [a-zA-Z$_] FIELDCHARS
     *
     * FIELDCHARS :
     *     [a-zA-Z0-9$_]
     *   | [a-zA-Z0-9$_] FIELDCHARS
     */
    Status field(std::string* result);

    /*
     * std::string :
     *     " "
     *   | ' '
     *   | " CHARS "
     *   | ' CHARS '
     */
    Status quotedString(std::string* result);

    /*
     * CHARS :
     *     CHAR
     *   | CHAR CHARS
     *
     * Note: " or ' may be allowed depending on whether the std::string is
     * double or single quoted
     *
     * CHAR :
     *     any-Unicode-character-except-"-or-'-or-\-or-CONTROLCHAR
     *   | \"
     *   | \'
     *   | \\
     *   | \/
     *   | \b
     *   | \f
     *   | \n
     *   | \r
     *   | \t
     *   | \v
     *   | \u HEXDIGIT HEXDIGIT HEXDIGIT HEXDIGIT
     *   | \any-Unicode-character-except-x-or-[0-9]
     *
     * HEXDIGIT : [0..9a..fA..F]
     *
     * per http://www.ietf.org/rfc/rfc4627.txt, control characters are
     * (U+0000 through U+001F).  U+007F is not mentioned as a control
     * character.
     * CONTROLCHAR : [0x00..0x1F]
     *
     * If there is not an error, result will contain a null terminated
     * string, but there is no guarantee that it will not contain other
     * null characters.
     */
    Status chars(std::string* result, const char* terminalSet, const char* allowedSet = nullptr);

    /**
     * Converts the two byte Unicode code point to its UTF8 character
     * encoding representation.  This function returns a std::string because
     * UTF8 encodings for code points from 0x0000 to 0xFFFF can range
     * from one to three characters.
     */
    std::string encodeUTF8(unsigned char first, unsigned char second) const;

    /**
     * @return true if the given token matches the next non whitespace
     * sequence in our buffer, and false if the token doesn't match or
     * we reach the end of our buffer.  Do not update the pointer to our
     * buffer (same as calling readTokenImpl with advance=false).
     */
    inline bool peekToken(std::string_view token);

    /**
     * @return true if the given token matches the next non whitespace
     * sequence in our buffer, and false if the token doesn't match or
     * we reach the end of our buffer.  Updates the pointer to our
     * buffer (same as calling readTokenImpl with advance=true).
     */
    [[nodiscard]] inline bool readToken(std::string_view token);

    /**
     * @return true if the given token matches the next non whitespace
     * sequence in our buffer, and false if the token doesn't match or
     * we reach the end of our buffer.  Do not update the pointer to our
     * buffer if advance is false.
     */
    bool readTokenImpl(std::string_view token, bool advance = true);

    /**
     * @return true if the next field in our stream matches field.
     * Handles single quoted, double quoted, and unquoted field names
     */
    bool readField(std::string_view field);

    /**
     * @return true if matchChar is in matchSet
     * @return true if matchSet is NULL and false if it is an empty string
     */
    bool match(char matchChar, const char* matchSet) const;

    /**
     * @return true if every character in the std::string is a hex digit
     */
    bool isHexString(std::string_view) const;

    /**
     * @return true if every character in the std::string is a valid base64
     * character
     */
    bool isBase64String(std::string_view) const;

    /**
     * Assumes there is a parse error at the current offset, appends a snippet of text from around
     * the bad input to 'errorBuffer'.
     */
    void addBadInputSnippet(std::ostringstream& errorBuffer) const;

    /**
     * Assumes there is a parse error at the current offset, appends the full input, then a newline,
     * then another line with a "^" character just below the offset of the problem. For example:
     * {$and: [{a: {$eq: 2}, {b: {$eq: 3}}]}
     *                       ^
     */
    void indicateOffsetPosition(std::ostringstream& errorBuffer) const;

    /**
     * @return FailedToParse status with the given message and some
     * additional context information
     */
    Status parseError(std::string_view msg);

    /**
     * @returns a valid Date_t or FailedToParse status.
     * Updates _input to past the end of the parsed date.
     */
    StatusWith<Date_t> parseDate();

public:
    inline int offset() const {
        return _input.data() - _buf.data();
    }

    inline int length() const {
        return _buf.size();
    }

private:
    /**
     * _buf is the buffer containing the JSON string we are parsing.
     * _input is the not-yet-parsed part of the buffer
     */
    std::string_view _buf;
    std::string_view _input;
};

}  // namespace mongo
