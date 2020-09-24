/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <boost/algorithm/string.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/parser_gen.hpp"
#include "mongo/util/string_map.h"

namespace mongo {

using namespace std::string_literals;

namespace {

// Mapping of reserved key fieldnames to BSON token. Any key which is not included in this map is
// assumed to be a user field name.
const StringMap<ParserGen::token_type> reservedKeyFieldnameLookup = {
    {"_id", ParserGen::token::ID},
    // Stages and their arguments.
    {"$_internalInhibitOptimization", ParserGen::token::STAGE_INHIBIT_OPTIMIZATION},
    {"$limit", ParserGen::token::STAGE_LIMIT},
    {"$project", ParserGen::token::STAGE_PROJECT},
    {"$sample", ParserGen::token::STAGE_SAMPLE},
    {"size", ParserGen::token::ARG_SIZE},
    {"$skip", ParserGen::token::STAGE_SKIP},
    {"$unionWith", ParserGen::token::STAGE_UNION_WITH},
    {"coll", ParserGen::token::ARG_COLL},
    {"pipeline", ParserGen::token::ARG_PIPELINE},
    // Expressions
    {"$abs", ParserGen::token::ABS},
    {"$acos", ParserGen::token::ACOS},
    {"$acosh", ParserGen::token::ACOSH},
    {"$add", ParserGen::token::ADD},
    {"$and", ParserGen::token::AND},
    {"$asin", ParserGen::token::ASIN},
    {"$asinh", ParserGen::token::ASINH},
    {"$atan", ParserGen::token::ATAN},
    {"$atan2", ParserGen::token::ATAN2},
    {"$atan2", ParserGen::token::ATAN2},
    {"$atanh", ParserGen::token::ATANH},
    {"$ceil", ParserGen::token::CEIL},
    {"$cmp", ParserGen::token::CMP},
    {"$concat", ParserGen::token::CONCAT},
    {"$const", ParserGen::token::CONST_EXPR},
    {"$convert", ParserGen::token::CONVERT},
    {"$cos", ParserGen::token::COS},
    {"$cosh", ParserGen::token::COSH},
    {"$dateFromString", ParserGen::token::DATE_FROM_STRING},
    {"$dateToString", ParserGen::token::DATE_TO_STRING},
    {"$degreesToRadians", ParserGen::token::DEGREES_TO_RADIANS},
    {"$divide", ParserGen::token::DIVIDE},
    {"$elemMatch", ParserGen::token::ELEM_MATCH},
    {"$eq", ParserGen::token::EQ},
    {"$exp", ParserGen::token::EXPONENT},
    {"$floor", ParserGen::token::FLOOR},
    {"$gt", ParserGen::token::GT},
    {"$gte", ParserGen::token::GTE},
    {"$indexOfBytes", ParserGen::token::INDEX_OF_BYTES},
    {"$indexOfCP", ParserGen::token::INDEX_OF_CP},
    {"$literal", ParserGen::token::LITERAL},
    {"$ln", ParserGen::token::LN},
    {"$log", ParserGen::token::LOG},
    {"$log10", ParserGen::token::LOGTEN},
    {"$lt", ParserGen::token::LT},
    {"$lte", ParserGen::token::LTE},
    {"$ltrim", ParserGen::token::LTRIM},
    {"$meta", ParserGen::token::META},
    {"$mod", ParserGen::token::MOD},
    {"$multiply", ParserGen::token::MULTIPLY},
    {"$ne", ParserGen::token::NE},
    {"$nor", ParserGen::token::NOR},
    {"$not", ParserGen::token::NOT},
    {"$or", ParserGen::token::OR},
    {"$pow", ParserGen::token::POW},
    {"$round", ParserGen::token::ROUND},
    {"$slice", ParserGen::token::SLICE},
    {"$sqrt", ParserGen::token::SQRT},
    {"$subtract", ParserGen::token::SUBTRACT},
    {"$trunc", ParserGen::token::TRUNC},
    {"$concat", ParserGen::token::CONCAT},
    {"$dateFromString", ParserGen::token::DATE_FROM_STRING},
    {"$dateToString", ParserGen::token::DATE_TO_STRING},
    {"$indexOfBytes", ParserGen::token::INDEX_OF_BYTES},
    {"$indexOfCP", ParserGen::token::INDEX_OF_CP},
    {"$ltrim", ParserGen::token::LTRIM},
    {"$meta", ParserGen::token::META},
    {"$radiansToDegrees", ParserGen::token::RADIANS_TO_DEGREES},
    {"$regexFind", ParserGen::token::REGEX_FIND},
    {"$regexFindAll", ParserGen::token::REGEX_FIND_ALL},
    {"$regexMatch", ParserGen::token::REGEX_MATCH},
    {"$replaceAll", ParserGen::token::REPLACE_ALL},
    {"$replaceOne", ParserGen::token::REPLACE_ONE},
    {"$round", ParserGen::token::ROUND},
    {"$rtrim", ParserGen::token::RTRIM},
    {"$sin", ParserGen::token::SIN},
    {"$sinh", ParserGen::token::SINH},
    {"$split", ParserGen::token::SPLIT},
    {"$sqrt", ParserGen::token::SQRT},
    {"$strcasecmp", ParserGen::token::STR_CASE_CMP},
    {"$strLenBytes", ParserGen::token::STR_LEN_BYTES},
    {"$strLenCP", ParserGen::token::STR_LEN_CP},
    {"$substr", ParserGen::token::SUBSTR},
    {"$substrBytes", ParserGen::token::SUBSTR_BYTES},
    {"$substrCP", ParserGen::token::SUBSTR_CP},
    {"$subtract", ParserGen::token::SUBTRACT},
    {"$tan", ParserGen::token::TAN},
    {"$tanh", ParserGen::token::TANH},
    {"$toBool", ParserGen::token::TO_BOOL},
    {"$toDate", ParserGen::token::TO_DATE},
    {"$toDecimal", ParserGen::token::TO_DECIMAL},
    {"$toDouble", ParserGen::token::TO_DOUBLE},
    {"$toInt", ParserGen::token::TO_INT},
    {"$toLong", ParserGen::token::TO_LONG},
    {"$toLower", ParserGen::token::TO_LOWER},
    {"$toObjectId", ParserGen::token::TO_OBJECT_ID},
    {"$toString", ParserGen::token::TO_STRING},
    {"$toUpper", ParserGen::token::TO_UPPER},
    {"$trim", ParserGen::token::TRIM},
    {"$trunc", ParserGen::token::TRUNC},
    {"$type", ParserGen::token::TYPE},
    {"chars", ParserGen::token::ARG_CHARS},
    {"date", ParserGen::token::ARG_DATE},
    {"$comment", ParserGen::token::COMMENT},
    {"$exists", ParserGen::token::EXISTS},
    {"dateString", ParserGen::token::ARG_DATE_STRING},
    {"find", ParserGen::token::ARG_FIND},
    {"format", ParserGen::token::ARG_FORMAT},
    {"input", ParserGen::token::ARG_INPUT},
    {"onError", ParserGen::token::ARG_ON_ERROR},
    {"onNull", ParserGen::token::ARG_ON_NULL},
    {"options", ParserGen::token::ARG_OPTIONS},
    {"find", ParserGen::token::ARG_FIND},
    {"regex", ParserGen::token::ARG_REGEX},
    {"replacement", ParserGen::token::ARG_REPLACEMENT},
    {"$allElementsTrue", ParserGen::token::ALL_ELEMENTS_TRUE},
    {"$anyElementTrue", ParserGen::token::ANY_ELEMENT_TRUE},
    {"$setDifference", ParserGen::token::SET_DIFFERENCE},
    {"$setEquals", ParserGen::token::SET_EQUALS},
    {"$setIntersection", ParserGen::token::SET_INTERSECTION},
    {"$setIsSubset", ParserGen::token::SET_IS_SUBSET},
    {"$setUnion", ParserGen::token::SET_UNION},
    {"timezone", ParserGen::token::ARG_TIMEZONE},
    {"to", ParserGen::token::ARG_TO},
    {"minute", ParserGen::token::ARG_MINUTE},
    {"second", ParserGen::token::ARG_SECOND},
    {"millisecond", ParserGen::token::ARG_MILLISECOND},
    {"day", ParserGen::token::ARG_DAY},
    {"isoDayOfWeek", ParserGen::token::ARG_ISO_DAY_OF_WEEK},
    {"isoWeek", ParserGen::token::ARG_ISO_WEEK},
    {"isoWeekYear", ParserGen::token::ARG_ISO_WEEK_YEAR},
    {"iso8601", ParserGen::token::ARG_ISO_8601},
    {"month", ParserGen::token::ARG_MONTH},
    {"year", ParserGen::token::ARG_YEAR},
    {"hour", ParserGen::token::ARG_HOUR},
    {"$dateFromParts", ParserGen::token::DATE_FROM_PARTS},
    {"$dateToParts", ParserGen::token::DATE_TO_PARTS},
    {"$dayOfMonth", ParserGen::token::DAY_OF_MONTH},
    {"$dayOfWeek", ParserGen::token::DAY_OF_WEEK},
    {"$dayOfYear", ParserGen::token::DAY_OF_YEAR},
    {"$hour", ParserGen::token::HOUR},
    {"$isoDayOfWeek", ParserGen::token::ISO_DAY_OF_WEEK},
    {"$isoWeek", ParserGen::token::ISO_WEEK},
    {"$isoWeekYear", ParserGen::token::ISO_WEEK_YEAR},
    {"$millisecond", ParserGen::token::MILLISECOND},
    {"$minute", ParserGen::token::MINUTE},
    {"$month", ParserGen::token::MONTH},
    {"$second", ParserGen::token::SECOND},
    {"$week", ParserGen::token::WEEK},
    {"$year", ParserGen::token::YEAR}};

// Mapping of reserved key values to BSON token. Any key which is not included in this map is
// assumed to be a user value.
const StringMap<ParserGen::token_type> reservedKeyValueLookup = {
    {"geoNearDistance", ParserGen::token::GEO_NEAR_DISTANCE},
    {"geoNearPoint", ParserGen::token::GEO_NEAR_POINT},
    {"indexKey", ParserGen::token::INDEX_KEY},
    {"randVal", ParserGen::token::RAND_VAL},
    {"recordId", ParserGen::token::RECORD_ID},
    {"searchHighlights", ParserGen::token::SEARCH_HIGHLIGHTS},
    {"searchScore", ParserGen::token::SEARCH_SCORE},
    {"sortKey", ParserGen::token::SORT_KEY},
    {"textScore", ParserGen::token::TEXT_SCORE},
};

bool isCompound(ParserGen::symbol_type token) {
    return token.type_get() == static_cast<int>(ParserGen::token::START_OBJECT) ||
        token.type_get() == static_cast<int>(ParserGen::token::START_ARRAY);
}

}  // namespace

void BSONLexer::sortObjTokens() {
    // A TokenElement is similar to a BSONElement, with the payload being a vector of Bison symbols
    // if the type is compound (object or array).
    using TokenElement = std::pair<ParserGen::symbol_type, std::vector<ParserGen::symbol_type>>;
    struct TokenElementCompare {
        bool operator()(const TokenElement& elem1, const TokenElement& elem2) const {
            return elem1.first.type_get() < elem2.first.type_get();
        }
    };

    auto currentPosition = _position;
    // Ensure that we've just entered an object - i.e. that the previous token was a START_OBJECT.
    // Otherwise, this function is a no-op.
    if (currentPosition < 1 ||
        _tokens[currentPosition - 1].type_get() !=
            static_cast<int>(ParserGen::token::START_OBJECT)) {
        return;
    }

    std::list<TokenElement> sortedTokenPairs;
    // We've just entered an object (i.e. the previous token was a start object). We will sort
    // tokens until the matching END_OBJECT is found.
    while (_tokens[currentPosition].type_get() != static_cast<int>(ParserGen::token::END_OBJECT)) {
        invariant(size_t(currentPosition) < _tokens.size());

        auto keyToken = _tokens[currentPosition++];

        std::vector<ParserGen::symbol_type> rhsTokens;
        rhsTokens.push_back(_tokens[currentPosition]);
        if (isCompound(_tokens[currentPosition])) {
            auto braceCount = 1;
            currentPosition++;
            // Only sort the top level tokens. If we encounter a compound type, then jump to its
            // matching bracket or brace.
            while (braceCount > 0) {
                if (isCompound(_tokens[currentPosition]))
                    braceCount++;
                if (_tokens[currentPosition].type_get() ==
                        static_cast<int>(ParserGen::token::END_OBJECT) ||
                    _tokens[currentPosition].type_get() ==
                        static_cast<int>(ParserGen::token::END_ARRAY))
                    braceCount--;

                rhsTokens.push_back(_tokens[currentPosition++]);
            }
        } else {
            // Scalar, already added above.
            currentPosition++;
        }
        sortedTokenPairs.push_back(std::make_pair(keyToken, rhsTokens));
    }
    sortedTokenPairs.sort(TokenElementCompare());

    // _position is at the token immediately following the initial START_OBJECT, and currentPosition
    // is at the matching END_OBJECT. We need to flatten the sorted list of KV pairs to get the
    // correct order of tokens.
    auto replacePosition = _position;
    for (auto&& [key, rhsTokens] : sortedTokenPairs) {
        _tokens[replacePosition].clear();
        _tokens[replacePosition++].move(key);
        for (auto&& token : rhsTokens) {
            _tokens[replacePosition].clear();
            _tokens[replacePosition++].move(token);
        }
    }
}

void BSONLexer::tokenize(BSONElement elem, bool includeFieldName) {
    boost::optional<ScopedLocationTracker> context;
    // Skipped when we are tokenizing arrays.
    if (includeFieldName) {
        if (auto it = reservedKeyFieldnameLookup.find(elem.fieldNameStringData());
            it != reservedKeyFieldnameLookup.end()) {
            // Place the token expected by the parser if this is a reserved key fieldname.
            pushToken(elem.fieldNameStringData(), it->second);
            context.emplace(this, elem.fieldNameStringData());
        } else if (elem.fieldNameStringData().find('.') != std::string::npos) {
            auto components = std::vector<std::string>{};
            boost::split(components, elem.fieldNameStringData(), [](auto&& c) { return c == '.'; });
            pushToken(elem.fieldNameStringData(),
                      ParserGen::token::DOTTED_FIELDNAME,
                      std::move(components));
        } else if (elem.fieldNameStringData()[0] == '$') {
            pushToken(elem.fieldNameStringData(),
                      ParserGen::token::DOLLAR_PREF_FIELDNAME,
                      elem.fieldName());
        } else {
            pushToken(elem.fieldNameStringData(), ParserGen::token::FIELDNAME, elem.fieldName());
        }
    }

    switch (elem.type()) {
        case BSONType::Array: {
            pushToken("start array", ParserGen::token::START_ARRAY);
            auto index = 0;
            for (auto&& nestedElem : elem.embeddedObject()) {
                ScopedLocationTracker arrayCtx{this, index++};
                // For arrays, do not tokenize the field names.
                tokenize(nestedElem, false);
            }
            pushToken("end array", ParserGen::token::END_ARRAY);
            break;
        }
        case BSONType::Object:
            pushToken("start object", ParserGen::token::START_OBJECT);
            for (auto&& nestedElem : elem.embeddedObject()) {
                tokenize(nestedElem, true);
            }
            pushToken("end object", ParserGen::token::END_OBJECT);
            break;
        case NumberDouble:
            if (elem.numberDouble() == 0.0)
                pushToken(elem, ParserGen::token::DOUBLE_ZERO);
            else if (elem.numberDouble() == 1.0)
                pushToken(elem, ParserGen::token::DOUBLE_ONE);
            else if (elem.numberDouble() == -1.0)
                pushToken(elem, ParserGen::token::DOUBLE_NEGATIVE_ONE);
            else
                pushToken(elem, ParserGen::token::DOUBLE_OTHER, elem.numberDouble());
            break;
        case BSONType::String:
            if (auto it = reservedKeyValueLookup.find(elem.valueStringData());
                it != reservedKeyValueLookup.end()) {
                // Place the token expected by the parser if this is a reserved key value.
                pushToken(elem.valueStringData(), it->second);
            } else {
                // If we don't care about the keyword, then it's treated as a generic value.
                if (elem.valueStringData()[0] == '$') {
                    if (elem.valueStringData()[1] == '$') {
                        pushToken(elem.valueStringData(),
                                  ParserGen::token::DOLLAR_DOLLAR_STRING,
                                  elem.String());
                    } else {
                        pushToken(
                            elem.valueStringData(), ParserGen::token::DOLLAR_STRING, elem.String());
                    }
                } else {
                    pushToken(elem.valueStringData(), ParserGen::token::STRING, elem.String());
                }
            }
            break;
        case BSONType::BinData: {
            int len;
            auto data = elem.binData(len);
            pushToken(elem, ParserGen::token::BINARY, BSONBinData{data, len, elem.binDataType()});
            break;
        }
        case BSONType::Undefined:
            pushToken(elem, ParserGen::token::UNDEFINED, UserUndefined{});
            break;
        case BSONType::jstOID:
            pushToken(elem, ParserGen::token::OBJECT_ID, elem.OID());
            break;
        case Bool:
            pushToken(elem,
                      elem.boolean() ? ParserGen::token::BOOL_TRUE : ParserGen::token::BOOL_FALSE);
            break;
        case BSONType::Date:
            pushToken(elem, ParserGen::token::DATE_LITERAL, elem.date());
            break;
        case BSONType::jstNULL:
            pushToken(elem, ParserGen::token::JSNULL, UserNull{});
            break;
        case BSONType::RegEx:
            pushToken(elem, ParserGen::token::REGEX, BSONRegEx{elem.regex(), elem.regexFlags()});
            break;
        case BSONType::DBRef:
            pushToken(
                elem, ParserGen::token::DB_POINTER, BSONDBRef{elem.dbrefNS(), elem.dbrefOID()});
            break;
        case BSONType::Code:
            pushToken(elem, ParserGen::token::JAVASCRIPT, BSONCode{elem.valueStringData()});
            break;
        case BSONType::Symbol:
            pushToken(elem, ParserGen::token::SYMBOL, BSONSymbol{elem.valueStringData()});
            break;
        case BSONType::CodeWScope: {
            auto code = StringData{elem.codeWScopeCode(),
                                   static_cast<size_t>(elem.codeWScopeCodeLen()) - 1ull};
            pushToken(elem,
                      ParserGen::token::JAVASCRIPT_W_SCOPE,
                      BSONCodeWScope{code, elem.codeWScopeObject()});
            break;
        }
        case NumberInt:
            if (elem.numberInt() == 0)
                pushToken(elem, ParserGen::token::INT_ZERO);
            else if (elem.numberInt() == 1)
                pushToken(elem, ParserGen::token::INT_ONE);
            else if (elem.numberInt() == -1)
                pushToken(elem, ParserGen::token::INT_NEGATIVE_ONE);
            else
                pushToken(elem, ParserGen::token::INT_OTHER, elem.numberInt());
            break;
        case BSONType::bsonTimestamp:
            pushToken(elem, ParserGen::token::TIMESTAMP, elem.timestamp());
            break;
        case NumberLong:
            if (elem.numberLong() == 0ll)
                pushToken(elem, ParserGen::token::LONG_ZERO);
            else if (elem.numberLong() == 1ll)
                pushToken(elem, ParserGen::token::LONG_ONE);
            else if (elem.numberLong() == -1ll)
                pushToken(elem, ParserGen::token::LONG_NEGATIVE_ONE);
            else
                pushToken(elem, ParserGen::token::LONG_OTHER, elem.numberLong());
            break;
        case NumberDecimal:
            if (elem.numberDecimal() == Decimal128::kNormalizedZero)
                pushToken(elem, ParserGen::token::DECIMAL_ZERO);
            else if (elem.numberDecimal() == Decimal128(1)) {
                pushToken(elem, ParserGen::token::DECIMAL_ONE);
            } else if (elem.numberDecimal() == Decimal128(-1)) {
                pushToken(elem, ParserGen::token::DECIMAL_NEGATIVE_ONE);
            } else
                pushToken(elem, ParserGen::token::DECIMAL_OTHER, elem.numberDecimal());
            break;
        case BSONType::MinKey:
            pushToken(elem, ParserGen::token::MIN_KEY, UserMinKey{});
            break;
        case BSONType::MaxKey:
            pushToken(elem, ParserGen::token::MAX_KEY, UserMaxKey{});
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

BSONLexer::BSONLexer(BSONObj obj, ParserGen::token_type startingToken) {
    // Add a prefix to the location depending on the starting token.
    ScopedLocationTracker inputPrefix{
        this,
        startingToken == ParserGen::token::START_PIPELINE
            ? "pipeline"
            : (startingToken == ParserGen::token::START_SORT
                   ? "sort"
                   : (startingToken == ParserGen::token::START_PROJECT ? "project" : "filter"))};
    pushToken("start", startingToken);

    // If 'obj' is representing a pipeline, each element is a stage with the fieldname being the
    // array index. No need to tokenize the fieldname for that case.
    if (startingToken == ParserGen::token::START_PIPELINE) {
        pushToken("start array", ParserGen::token::START_ARRAY);
        auto index = 0;
        for (auto&& elem : obj) {
            ScopedLocationTracker stageCtx{this, index++};
            tokenize(elem, false);
        }
        pushToken("end array", ParserGen::token::END_ARRAY);
    } else {
        pushToken("start object", ParserGen::token::START_OBJECT);
        for (auto&& elem : obj) {
            tokenize(elem, true);
        }
        pushToken("end object", ParserGen::token::END_OBJECT);
    }

    // Final token must indicate EOF.
    pushToken("EOF", ParserGen::token::END_OF_FILE);

    // Reset the position to use in yylex().
    _position = 0;
};

ParserGen::symbol_type yylex(mongo::BSONLexer& lexer) {
    return lexer.getNext();
}

}  // namespace mongo
