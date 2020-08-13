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

#include "mongo/base/string_data.h"
#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/pipeline_parser_gen.hpp"
#include "mongo/util/string_map.h"

namespace mongo {

using namespace std::string_literals;

namespace {

// Mapping of reserved keywords to BSON token. Any key which is not included in this map is assumed
// to be a user field name and is treated as a terminal by the parser.
const StringMap<PipelineParserGen::token_type> reservedKeyLookup = {
    {"_id", PipelineParserGen::token::ID},
    // Stages and their arguments.
    {"$_internalInhibitOptimization", PipelineParserGen::token::STAGE_INHIBIT_OPTIMIZATION},
    {"$limit", PipelineParserGen::token::STAGE_LIMIT},
    {"$project", PipelineParserGen::token::STAGE_PROJECT},
    {"$sample", PipelineParserGen::token::STAGE_SAMPLE},
    {"size", PipelineParserGen::token::ARG_SIZE},
    {"$skip", PipelineParserGen::token::STAGE_SKIP},
    {"$unionWith", PipelineParserGen::token::STAGE_UNION_WITH},
    {"coll", PipelineParserGen::token::ARG_COLL},
    {"pipeline", PipelineParserGen::token::ARG_PIPELINE},
    // Expressions
    {"$add", PipelineParserGen::token::ADD},
    {"$atan2", PipelineParserGen::token::ATAN2},
    {"$and", PipelineParserGen::token::AND},
    {"$or", PipelineParserGen::token::OR},
    {"$not", PipelineParserGen::token::NOT},
    {"$const", PipelineParserGen::token::CONST_EXPR},
    {"$literal", PipelineParserGen::token::LITERAL},
    {"$cmp", PipelineParserGen::token::CMP},
    {"$eq", PipelineParserGen::token::EQ},
    {"$gt", PipelineParserGen::token::GT},
    {"$gte", PipelineParserGen::token::GTE},
    {"$lt", PipelineParserGen::token::LT},
    {"$lte", PipelineParserGen::token::LTE},
    {"$ne", PipelineParserGen::token::NE},
    {"$convert", PipelineParserGen::token::CONVERT},
    {"input", PipelineParserGen::token::ARG_INPUT},
    {"to", PipelineParserGen::token::ARG_TO},
    {"onError", PipelineParserGen::token::ARG_ON_ERROR},
    {"onNull", PipelineParserGen::token::ARG_ON_NULL},
    {"$toBool", PipelineParserGen::token::TO_BOOL},
    {"$toDate", PipelineParserGen::token::TO_DATE},
    {"$toDecimal", PipelineParserGen::token::TO_DECIMAL},
    {"$toDouble", PipelineParserGen::token::TO_DOUBLE},
    {"$toInt", PipelineParserGen::token::TO_INT},
    {"$toLong", PipelineParserGen::token::TO_LONG},
    {"$toObjectId", PipelineParserGen::token::TO_OBJECT_ID},
    {"$toString", PipelineParserGen::token::TO_STRING},
    {"$type", PipelineParserGen::token::TYPE},
    {"$abs", PipelineParserGen::token::ABS},
    {"$ceil", PipelineParserGen::token::CEIL},
    {"$divide", PipelineParserGen::token::DIVIDE},
    {"$exp", PipelineParserGen::token::EXPONENT},
    {"$floor", PipelineParserGen::token::FLOOR},
    {"$ln", PipelineParserGen::token::LN},
    {"$log", PipelineParserGen::token::LOG},
    {"$log10", PipelineParserGen::token::LOGTEN},
    {"$mod", PipelineParserGen::token::MOD},
    {"$multiply", PipelineParserGen::token::MULTIPLY},
    {"$pow", PipelineParserGen::token::POW},
    {"$round", PipelineParserGen::token::ROUND},
    {"$sqrt", PipelineParserGen::token::SQRT},
    {"$subtract", PipelineParserGen::token::SUBTRACT},
    {"$trunc", PipelineParserGen::token::TRUNC},
    {"$concat", PipelineParserGen::token::CONCAT},
    {"$dateFromString", PipelineParserGen::token::DATE_FROM_STRING},
    {"$dateToString", PipelineParserGen::token::DATE_TO_STRING},
    {"$indexOfBytes", PipelineParserGen::token::INDEX_OF_BYTES},
    {"$indexOfCP", PipelineParserGen::token::INDEX_OF_CP},
    {"$ltrim", PipelineParserGen::token::LTRIM},
    {"$regexFind", PipelineParserGen::token::REGEX_FIND},
    {"$regexFindAll", PipelineParserGen::token::REGEX_FIND_ALL},
    {"$regexMatch", PipelineParserGen::token::REGEX_MATCH},
    {"$replaceOne", PipelineParserGen::token::REPLACE_ONE},
    {"$replaceAll", PipelineParserGen::token::REPLACE_ALL},
    {"$rtrim", PipelineParserGen::token::RTRIM},
    {"$split", PipelineParserGen::token::SPLIT},
    {"$strLenBytes", PipelineParserGen::token::STR_LEN_BYTES},
    {"$strLenCP", PipelineParserGen::token::STR_LEN_CP},
    {"$strcasecmp", PipelineParserGen::token::STR_CASE_CMP},
    {"$substr", PipelineParserGen::token::SUBSTR},
    {"$substrBytes", PipelineParserGen::token::SUBSTR_BYTES},
    {"$substrCP", PipelineParserGen::token::SUBSTR_CP},
    {"$toLower", PipelineParserGen::token::TO_LOWER},
    {"$trim", PipelineParserGen::token::TRIM},
    {"$toUpper", PipelineParserGen::token::TO_UPPER},
    {"dateString", PipelineParserGen::token::ARG_DATE_STRING},
    {"format", PipelineParserGen::token::ARG_FORMAT},
    {"timezone", PipelineParserGen::token::ARG_TIMEZONE},
    {"date", PipelineParserGen::token::ARG_DATE},
    {"chars", PipelineParserGen::token::ARG_CHARS},
    {"regex", PipelineParserGen::token::ARG_REGEX},
    {"options", PipelineParserGen::token::ARG_OPTIONS},
    {"find", PipelineParserGen::token::ARG_FIND},
    {"replacement", PipelineParserGen::token::ARG_REPLACEMENT},
};
bool isCompound(PipelineParserGen::symbol_type token) {
    return token.type_get() == static_cast<int>(PipelineParserGen::token::START_OBJECT) ||
        token.type_get() == static_cast<int>(PipelineParserGen::token::START_ARRAY);
}

}  // namespace

void BSONLexer::sortObjTokens() {
    // A TokenElement is similar to a BSONElement, with the payload being a vector of Bison symbols
    // if the type is compound (object or array).
    using TokenElement =
        std::pair<PipelineParserGen::symbol_type, std::vector<PipelineParserGen::symbol_type>>;
    struct TokenElementCompare {
        bool operator()(const TokenElement& elem1, const TokenElement& elem2) const {
            return elem1.first.type_get() < elem2.first.type_get();
        }
    };

    auto currentPosition = _position;
    if (_tokens[currentPosition].type_get() !=
        static_cast<int>(PipelineParserGen::token::START_OBJECT)) {
        return;
    }

    std::list<TokenElement> sortedTokenPairs;
    // Increment to get to the first token after the START_OBJECT. We will sort tokens until the
    // matching END_OBJECT is found.
    currentPosition++;
    while (_tokens[currentPosition].type_get() !=
           static_cast<int>(PipelineParserGen::token::END_OBJECT)) {
        invariant(size_t(currentPosition) < _tokens.size());

        auto keyToken = _tokens[currentPosition++];

        std::vector<PipelineParserGen::symbol_type> rhsTokens;
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
                        static_cast<int>(PipelineParserGen::token::END_OBJECT) ||
                    _tokens[currentPosition].type_get() ==
                        static_cast<int>(PipelineParserGen::token::END_ARRAY))
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

    // _position is at the initial START_OBJECT, and currentPosition is at its matching
    // END_OBJECT. We need to flatten the sorted list of KV pairs to get the correct order of
    // tokens.
    auto replacePosition = _position + 1;
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
        if (auto it = reservedKeyLookup.find(elem.fieldNameStringData());
            it != reservedKeyLookup.end()) {
            // Place the token expected by the parser if this is a reserved keyword.
            pushToken(elem.fieldNameStringData(), it->second);
            context.emplace(this, elem.fieldNameStringData());
        } else if (elem.fieldNameStringData()[0] == '$') {
            pushToken(elem.fieldNameStringData(),
                      PipelineParserGen::token::DOLLAR_PREF_FIELDNAME,
                      elem.fieldName());
        } else {
            // If we don't care about the keyword, then it's treated as a generic fieldname.
            pushToken(
                elem.fieldNameStringData(), PipelineParserGen::token::FIELDNAME, elem.fieldName());
        }
    }

    switch (elem.type()) {
        case BSONType::Array: {
            pushToken("start array", PipelineParserGen::token::START_ARRAY);
            auto index = 0;
            for (auto&& nestedElem : elem.Array()) {
                ScopedLocationTracker arrayCtx{this, index++};
                // For arrays, do not tokenize the field names.
                tokenize(nestedElem, false);
            }
            pushToken("end array", PipelineParserGen::token::END_ARRAY);
            break;
        }
        case BSONType::Object:
            pushToken("start object", PipelineParserGen::token::START_OBJECT);
            for (auto&& nestedElem : elem.embeddedObject()) {
                tokenize(nestedElem, true);
            }
            pushToken("end object", PipelineParserGen::token::END_OBJECT);
            break;
        case NumberDouble:
            if (elem.numberDouble() == 0.0)
                pushToken(elem, PipelineParserGen::token::DOUBLE_ZERO);
            else
                pushToken(elem, PipelineParserGen::token::DOUBLE_NON_ZERO, elem.numberDouble());
            break;
        case BSONType::String:
            if (elem.valueStringData()[0] == '$') {
                if (elem.valueStringData()[1] == '$') {
                    pushToken(elem.valueStringData(),
                              PipelineParserGen::token::DOLLAR_DOLLAR_STRING,
                              elem.String());
                } else {
                    pushToken(elem.valueStringData(),
                              PipelineParserGen::token::DOLLAR_STRING,
                              elem.String());
                }
            } else {
                pushToken(elem.valueStringData(), PipelineParserGen::token::STRING, elem.String());
            }
            break;
        case BSONType::BinData: {
            int len;
            auto data = elem.binData(len);
            pushToken(
                elem, PipelineParserGen::token::BINARY, BSONBinData{data, len, elem.binDataType()});
            break;
        }
        case BSONType::Undefined:
            pushToken(elem, PipelineParserGen::token::UNDEFINED, UserUndefined{});
            break;
        case BSONType::jstOID:
            pushToken(elem, PipelineParserGen::token::OBJECT_ID, elem.OID());
            break;
        case Bool:
            pushToken(elem,
                      elem.boolean() ? PipelineParserGen::token::BOOL_TRUE
                                     : PipelineParserGen::token::BOOL_FALSE);
            break;
        case BSONType::Date:
            pushToken(elem, PipelineParserGen::token::DATE_LITERAL, elem.date());
            break;
        case BSONType::jstNULL:
            pushToken(elem, PipelineParserGen::token::JSNULL, UserNull{});
            break;
        case BSONType::RegEx:
            pushToken(
                elem, PipelineParserGen::token::REGEX, BSONRegEx{elem.regex(), elem.regexFlags()});
            break;
        case BSONType::DBRef:
            pushToken(elem,
                      PipelineParserGen::token::DB_POINTER,
                      BSONDBRef{elem.dbrefNS(), elem.dbrefOID()});
            break;
        case BSONType::Code:
            pushToken(elem, PipelineParserGen::token::JAVASCRIPT, BSONCode{elem.valueStringData()});
            break;
        case BSONType::Symbol:
            pushToken(elem, PipelineParserGen::token::SYMBOL, BSONSymbol{elem.valueStringData()});
            break;
        case BSONType::CodeWScope: {
            auto code = StringData{elem.codeWScopeCode(),
                                   static_cast<size_t>(elem.codeWScopeCodeLen()) - 1ull};
            pushToken(elem,
                      PipelineParserGen::token::JAVASCRIPT_W_SCOPE,
                      BSONCodeWScope{code, elem.codeWScopeObject()});
            break;
        }
        case NumberInt:
            if (elem.numberInt() == 0)
                pushToken(elem, PipelineParserGen::token::INT_ZERO);
            else
                pushToken(elem, PipelineParserGen::token::INT_NON_ZERO, elem.numberInt());
            break;
        case BSONType::bsonTimestamp:
            pushToken(elem, PipelineParserGen::token::TIMESTAMP, elem.timestamp());
            break;
        case NumberLong:
            if (elem.numberLong() == 0ll)
                pushToken(elem, PipelineParserGen::token::LONG_ZERO);
            else
                pushToken(elem, PipelineParserGen::token::LONG_NON_ZERO, elem.numberLong());
            break;
        case NumberDecimal:
            if (elem.numberDecimal() == Decimal128::kNormalizedZero)
                pushToken(elem, PipelineParserGen::token::DECIMAL_ZERO);
            else
                pushToken(elem, PipelineParserGen::token::DECIMAL_NON_ZERO, elem.numberDecimal());
            break;
        case BSONType::MinKey:
            pushToken(elem, PipelineParserGen::token::MIN_KEY, UserMinKey{});
            break;
        case BSONType::MaxKey:
            pushToken(elem, PipelineParserGen::token::MAX_KEY, UserMaxKey{});
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

BSONLexer::BSONLexer(BSONObj obj, PipelineParserGen::token_type startingToken) {
    ScopedLocationTracker matchCtx{this, "filter"};
    pushToken("start", startingToken);
    pushToken("start object", PipelineParserGen::token::START_OBJECT);
    for (auto&& elem : obj) {
        // Include field names in the object.
        tokenize(elem, true);
    }
    pushToken("end object", PipelineParserGen::token::END_OBJECT);

    // Final token must indicate EOF.
    pushToken("EOF", PipelineParserGen::token::END_OF_FILE);

    // Reset the position to use in yylex().
    _position = 0;
};

BSONLexer::BSONLexer(std::vector<BSONElement> pipeline,
                     PipelineParserGen::token_type startingToken) {
    ScopedLocationTracker pipelineCtx{this, "pipeline"};
    pushToken("start", startingToken);
    pushToken("start array", PipelineParserGen::token::START_ARRAY);
    auto index = 0;
    for (auto&& elem : pipeline) {
        ScopedLocationTracker stageCtx{this, index++};
        // Don't include field names for stages of the pipeline (aka indexes of the pipeline array).
        tokenize(elem, false);
    }
    pushToken("end array", PipelineParserGen::token::END_ARRAY);

    // Final token must indicate EOF.
    pushToken("EOF", PipelineParserGen::token::END_OF_FILE);

    // Reset the position to use in yylex().
    _position = 0;
};

PipelineParserGen::symbol_type yylex(mongo::BSONLexer& lexer) {
    return lexer.getNext();
}

}  // namespace mongo
