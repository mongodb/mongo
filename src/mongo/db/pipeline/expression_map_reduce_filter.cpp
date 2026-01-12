/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

/* ------------------------- ExpressionMap ----------------------------- */

REGISTER_STABLE_EXPRESSION(map, ExpressionMap::parse);
boost::intrusive_ptr<Expression> ExpressionMap::parse(ExpressionContext* const expCtx,
                                                      BSONElement expr,
                                                      const VariablesParseState& vpsIn) {
    MONGO_verify(expr.fieldNameStringData() == "$map");

    uassert(16878, "$map only supports an object as its argument", expr.type() == BSONType::object);

    const bool isExposeArrayIndexEnabled = expCtx->shouldParserIgnoreFeatureFlagCheck() ||
        feature_flags::gFeatureFlagExposeArrayIndexInMapFilterReduce
            .isEnabledUseLastLTSFCVWhenUninitialized(
                expCtx->getVersionContext(),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    // "in" must be parsed after "as" regardless of BSON order.
    BSONElement inputElem;
    BSONElement asElem;
    BSONElement inElem;
    BSONElement arrayIndexAsElem;
    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        if (arg.fieldNameStringData() == "input") {
            inputElem = arg;
        } else if (arg.fieldNameStringData() == "as") {
            asElem = arg;
        } else if (arg.fieldNameStringData() == "in") {
            inElem = arg;
        } else if (isExposeArrayIndexEnabled && arg.fieldNameStringData() == "arrayIndexAs") {
            assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                           "arrayIndexAs argument of $map operator",
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny);
            arrayIndexAsElem = arg;
        } else {
            uasserted(16879,
                      str::stream() << "Unrecognized parameter to $map: " << arg.fieldName());
        }
    }

    uassert(16880, "Missing 'input' parameter to $map", !inputElem.eoo());
    uassert(16882, "Missing 'in' parameter to $map", !inElem.eoo());

    // "vpsSub" gets our variables, "vpsIn" doesn't.
    VariablesParseState vpsSub(vpsIn);

    // Parse "as". If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    variableValidation::validateNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // Parse "arrayIndexAs". If "arrayIndexAs" is not specified, then write to "IDX" by default.
    boost::optional<std::string> idxName;
    boost::optional<Variables::Id> idxId;
    if (isExposeArrayIndexEnabled) {
        if (arrayIndexAsElem) {
            idxName = arrayIndexAsElem.str();
            variableValidation::validateNameForUserWrite(*idxName);

            uassert(
                9375801, "'as' and 'arrayIndexAs' cannot have the same name", varName != idxName);
        }
        idxId = vpsSub.defineVariable(!idxName ? "IDX" : *idxName);
    }

    return make_intrusive<ExpressionMap>(
        expCtx,
        std::move(varName),
        varId,
        std::move(idxName),
        idxId,
        parseOperand(expCtx, inputElem, vpsIn),  // Only has access to outer vars.
        parseOperand(expCtx, inElem, vpsSub)     // Has access to "as" and "arrayIndexAs" vars.
    );
}

ExpressionMap::ExpressionMap(ExpressionContext* const expCtx,
                             const std::string& varName,
                             Variables::Id varId,
                             const boost::optional<std::string>& idxName,
                             const boost::optional<Variables::Id>& idxId,
                             boost::intrusive_ptr<Expression> input,
                             boost::intrusive_ptr<Expression> each)
    : Expression(expCtx, {std::move(input), std::move(each)}),
      _varName(varName),
      _varId(varId),
      _idxName(std::move(idxName)),
      _idxId(idxId) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

boost::intrusive_ptr<Expression> ExpressionMap::optimize() {
    // TODO(SERVER-111215) handle when _input is constant
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kEach] = _children[_kEach]->optimize();
    return this;
}

Value ExpressionMap::serialize(const SerializationOptions& options) const {
    return Value(
        Document{{"$map",
                  Document{{"input", _children[_kInput]->serialize(options)},
                           {"as", options.serializeIdentifier(_varName)},
                           {"arrayIndexAs",
                            _idxName ? Value(options.serializeIdentifier(*_idxName)) : Value()},
                           {"in", _children[_kEach]->serialize(options)}}}});
}

Value ExpressionMap::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ------------------------ ExpressionReduce ------------------------------ */

REGISTER_STABLE_EXPRESSION(reduce, ExpressionReduce::parse);
boost::intrusive_ptr<Expression> ExpressionReduce::parse(ExpressionContext* const expCtx,
                                                         BSONElement expr,
                                                         const VariablesParseState& vps) {
    uassert(40075,
            str::stream() << "$reduce requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == BSONType::object);

    const bool isExposeArrayIndexEnabled = expCtx->shouldParserIgnoreFeatureFlagCheck() ||
        feature_flags::gFeatureFlagExposeArrayIndexInMapFilterReduce
            .isEnabledUseLastLTSFCVWhenUninitialized(
                expCtx->getVersionContext(),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    BSONElement inputElem;
    BSONElement initialElem;
    BSONElement inElem;
    BSONElement asElem;
    BSONElement valueAsElem;
    BSONElement arrayIndexAsElem;

    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();
        if (field == "input") {
            inputElem = elem;
        } else if (field == "initialValue") {
            initialElem = elem;
        } else if (field == "in") {
            inElem = elem;
        } else if (isExposeArrayIndexEnabled && field == "as") {
            assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                           "as argument of $reduce operator",
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny);
            asElem = elem;
        } else if (isExposeArrayIndexEnabled && field == "valueAs") {
            assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                           "valueAs argument of $reduce operator",
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny);
            valueAsElem = elem;
        } else if (isExposeArrayIndexEnabled && field == "arrayIndexAs") {
            assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                           "arrayIndexAs argument of $reduce operator",
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny);
            arrayIndexAsElem = elem;
        } else {
            uasserted(40076, str::stream() << "$reduce found an unknown argument: " << field);
        }
    }
    uassert(40077, "$reduce requires 'input' to be specified", inputElem);
    uassert(40078, "$reduce requires 'initialValue' to be specified", initialElem);
    uassert(40079, "$reduce requires 'in' to be specified", inElem);

    // "vpsSub" gets our variables, "vps" doesn't.
    VariablesParseState vpsSub(vps);

    auto parseVariableDefinition = [&vpsSub](const BSONElement& elem, StringData defaultName) {
        boost::optional<std::string> name;
        if (elem) {
            name = elem.str();
            variableValidation::validateNameForUserWrite(*name);
        }
        Variables::Id id = vpsSub.defineVariable(!name ? defaultName : *name);
        return std::make_pair(name, id);
    };

    // Parse "as". If is not specified, use "this" by default.
    boost::optional<std::string> thisName;
    Variables::Id thisId;
    if (isExposeArrayIndexEnabled) {
        std::tie(thisName, thisId) = parseVariableDefinition(asElem, "this");
    } else {
        // Keep previous behavior if feature flag is disabled.
        thisId = vpsSub.defineVariable("this");
    }

    // Parse "valueAs". If is not specified, use "value" by default.
    boost::optional<std::string> valueName;
    Variables::Id valueId;
    if (isExposeArrayIndexEnabled) {
        std::tie(valueName, valueId) = parseVariableDefinition(valueAsElem, "value");
    } else {
        // Keep previous behavior if feature flag is disabled.
        valueId = vpsSub.defineVariable("value");
    }

    // Parse "arrayIndexAs". If is not specified, use "IDX" by default.
    boost::optional<std::string> idxName;
    boost::optional<Variables::Id> idxId;
    if (isExposeArrayIndexEnabled) {
        std::tie(idxName, idxId) = parseVariableDefinition(arrayIndexAsElem, "IDX");
    }

    // Validate uniqueness of the user-defined variables.
    boost::optional<std::string> repeatedName;
    if (thisName && (thisName == valueName || thisName == idxName)) {
        repeatedName = *thisName;
    } else if (valueName && (valueName == idxName)) {
        repeatedName = *valueName;
    }

    uassert(9298401,
            str::stream() << "Cannot define variables with the same name " << *repeatedName,
            !repeatedName.has_value());

    return make_intrusive<ExpressionReduce>(expCtx,
                                            parseOperand(expCtx, inputElem, vps),
                                            parseOperand(expCtx, initialElem, vps),
                                            parseOperand(expCtx, inElem, vpsSub),
                                            std::move(idxName),
                                            idxId,
                                            std::move(thisName),
                                            thisId,
                                            std::move(valueName),
                                            valueId);
}

Value ExpressionReduce::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionReduce::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kInitial] = _children[_kInitial]->optimize();
    _children[_kIn] = _children[_kIn]->optimize();
    return this;
}

Value ExpressionReduce::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {"$reduce",
         Document{
             {"input", _children[_kInput]->serialize(options)},
             {"initialValue", _children[_kInitial]->serialize(options)},
             {"as", _thisName ? Value(options.serializeIdentifier(*_thisName)) : Value()},
             {"valueAs", _valueName ? Value(options.serializeIdentifier(*_valueName)) : Value()},
             {"arrayIndexAs", _idxName ? Value(options.serializeIdentifier(*_idxName)) : Value()},
             {"in", _children[_kIn]->serialize(options)}}}});
}

/* ------------------------- ExpressionFilter ----------------------------- */

REGISTER_STABLE_EXPRESSION(filter, ExpressionFilter::parse);
boost::intrusive_ptr<Expression> ExpressionFilter::parse(ExpressionContext* const expCtx,
                                                         BSONElement expr,
                                                         const VariablesParseState& vpsIn) {
    MONGO_verify(expr.fieldNameStringData() == "$filter");

    uassert(
        28646, "$filter only supports an object as its argument", expr.type() == BSONType::object);

    const bool isExposeArrayIndexEnabled = expCtx->shouldParserIgnoreFeatureFlagCheck() ||
        feature_flags::gFeatureFlagExposeArrayIndexInMapFilterReduce
            .isEnabledUseLastLTSFCVWhenUninitialized(
                expCtx->getVersionContext(),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    // "cond" must be parsed after "as" regardless of BSON order.
    BSONElement inputElem;
    BSONElement asElem;
    BSONElement condElem;
    BSONElement limitElem;
    BSONElement arrayIndexAsElem;

    for (auto elem : expr.Obj()) {
        if (elem.fieldNameStringData() == "input") {
            inputElem = elem;
        } else if (elem.fieldNameStringData() == "as") {
            asElem = elem;
        } else if (elem.fieldNameStringData() == "cond") {
            condElem = elem;
        } else if (elem.fieldNameStringData() == "limit") {
            assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                           "limit argument of $filter operator",
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny);
            limitElem = elem;
        } else if (isExposeArrayIndexEnabled && elem.fieldNameStringData() == "arrayIndexAs") {
            assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                           "arrayIndexAs argument of $filter operator",
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny);
            arrayIndexAsElem = elem;
        } else {
            uasserted(28647,
                      str::stream() << "Unrecognized parameter to $filter: " << elem.fieldName());
        }
    }

    uassert(28648, "Missing 'input' parameter to $filter", !inputElem.eoo());
    uassert(28650, "Missing 'cond' parameter to $filter", !condElem.eoo());

    // "vpsSub" gets our variables, "vpsIn" doesn't.
    VariablesParseState vpsSub(vpsIn);

    // Parse "as". If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    variableValidation::validateNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // Parse "arrayIndexAs". If "arrayIndexAs" is not specified, then write to "IDX" by default.
    boost::optional<std::string> idxName;
    boost::optional<Variables::Id> idxId;
    if (isExposeArrayIndexEnabled) {
        if (arrayIndexAsElem) {
            idxName = arrayIndexAsElem.str();
            variableValidation::validateNameForUserWrite(*idxName);

            uassert(9375802,
                    "Can't redefine variable specified in 'as' and 'arrayIndexAs' parameters",
                    varName != idxName);
        }
        idxId = vpsSub.defineVariable(!idxName ? "IDX" : *idxName);
    }

    boost::intrusive_ptr<Expression> limit;
    if (limitElem) {
        limit = parseOperand(expCtx, limitElem, vpsIn);
    }
    return make_intrusive<ExpressionFilter>(
        expCtx,
        std::move(varName),
        varId,
        std::move(idxName),
        idxId,
        parseOperand(expCtx, inputElem, vpsIn),  // Only has access to outer vars.
        parseOperand(expCtx, condElem, vpsSub),  // Has access to "as" and "arrayIndexAs" vars.
        std::move(limit));
}

ExpressionFilter::ExpressionFilter(ExpressionContext* const expCtx,
                                   std::string varName,
                                   Variables::Id varId,
                                   const boost::optional<std::string>& idxName,
                                   const boost::optional<Variables::Id>& idxId,
                                   boost::intrusive_ptr<Expression> input,
                                   boost::intrusive_ptr<Expression> cond,
                                   boost::intrusive_ptr<Expression> limit)
    : Expression(expCtx,
                 limit ? makeVector(std::move(input), std::move(cond), std::move(limit))
                       : makeVector(std::move(input), std::move(cond))),
      _varName(std::move(varName)),
      _varId(varId),
      _idxName(std::move(idxName)),
      _idxId(idxId),
      _limit(_children.size() == 3 ? 2 : boost::optional<size_t>(boost::none)) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

boost::intrusive_ptr<Expression> ExpressionFilter::optimize() {
    // TODO(SERVER-111215) handle when _input is constant.
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kCond] = _children[_kCond]->optimize();
    if (_limit)
        _children[*_limit] = (_children[*_limit])->optimize();

    return this;
}

Value ExpressionFilter::serialize(const SerializationOptions& options) const {
    return Value(
        Document{{"$filter",
                  Document{{"input", _children[_kInput]->serialize(options)},
                           {"as", options.serializeIdentifier(_varName)},
                           {"arrayIndexAs",
                            _idxName ? Value(options.serializeIdentifier(*_idxName)) : Value()},
                           {"cond", _children[_kCond]->serialize(options)},
                           {"limit", _limit ? _children[*_limit]->serialize(options) : Value()}}}});
}

Value ExpressionFilter::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

}  // namespace mongo
