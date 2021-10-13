
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


#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/expression.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cstdio>
#include <vector>

#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/summation.h"

namespace mongo {
using Parser = Expression::Parser;

using namespace mongoutils;

using boost::intrusive_ptr;
using std::map;
using std::move;
using std::pair;
using std::set;
using std::string;
using std::vector;

/// Helper function to easily wrap constants with $const.
static Value serializeConstant(Value val) {
    if (val.missing()) {
        return Value("$$REMOVE"_sd);
    }

    return Value(DOC("$const" << val));
}

/* --------------------------- Expression ------------------------------ */

string Expression::removeFieldPrefix(const string& prefixedField) {
    uassert(16419,
            str::stream() << "field path must not contain embedded null characters"
                          << prefixedField.find("\0")
                          << ",",
            prefixedField.find('\0') == string::npos);

    const char* pPrefixedField = prefixedField.c_str();
    uassert(15982,
            str::stream() << "field path references must be prefixed with a '$' ('" << prefixedField
                          << "'",
            pPrefixedField[0] == '$');

    return string(pPrefixedField + 1);
}

intrusive_ptr<Expression> Expression::parseObject(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONObj obj,
    const VariablesParseState& vps) {
    if (obj.isEmpty()) {
        return ExpressionObject::create(expCtx, {});
    }

    if (obj.firstElementFieldName()[0] == '$') {
        // Assume this is an expression like {$add: [...]}.
        return parseExpression(expCtx, obj, vps);
    }

    return ExpressionObject::parse(expCtx, obj, vps);
}

namespace {
struct ParserRegistration {
    Parser parser;
    boost::optional<ServerGlobalParams::FeatureCompatibility::Version> requiredMinVersion;
};

StringMap<ParserRegistration> parserMap;
}

void Expression::registerExpression(
    string key,
    Parser parser,
    boost::optional<ServerGlobalParams::FeatureCompatibility::Version> requiredMinVersion) {
    auto op = parserMap.find(key);
    massert(17064,
            str::stream() << "Duplicate expression (" << key << ") registered.",
            op == parserMap.end());
    parserMap[key] = {parser, requiredMinVersion};
}

intrusive_ptr<Expression> Expression::parseExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONObj obj,
    const VariablesParseState& vps) {
    uassert(15983,
            str::stream() << "An object representing an expression must have exactly one "
                             "field: "
                          << obj.toString(),
            obj.nFields() == 1);

    // Look up the parser associated with the expression name.
    const char* opName = obj.firstElementFieldName();
    auto it = parserMap.find(opName);
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Unrecognized expression '" << opName << "'",
            it != parserMap.end());

    // Make sure we are allowed to use this expression under the current feature compatibility
    // version.
    auto& entry = it->second;
    uassert(
        ErrorCodes::QueryFeatureNotAllowed,
        // TODO SERVER-31968 we would like to include the current version and the required minimum
        // version in this error message, but using FeatureCompatibilityVersion::toString() would
        // introduce a dependency cycle.
        str::stream() << opName
                      << " is not allowed in the current feature compatibility version. See "
                      << feature_compatibility_version_documentation::kCompatibilityLink
                      << " for more information.",
        !expCtx->maxFeatureCompatibilityVersion || !entry.requiredMinVersion ||
            (*entry.requiredMinVersion <= *expCtx->maxFeatureCompatibilityVersion));
    return entry.parser(expCtx, obj.firstElement(), vps);
}

Expression::ExpressionVector ExpressionNary::parseArguments(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement exprElement,
    const VariablesParseState& vps) {
    ExpressionVector out;
    if (exprElement.type() == Array) {
        BSONForEach(elem, exprElement.Obj()) {
            out.push_back(Expression::parseOperand(expCtx, elem, vps));
        }
    } else {  // Assume it's an operand that accepts a single argument.
        out.push_back(Expression::parseOperand(expCtx, exprElement, vps));
    }

    return out;
}

intrusive_ptr<Expression> Expression::parseOperand(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement exprElement,
    const VariablesParseState& vps) {
    BSONType type = exprElement.type();

    if (type == String && exprElement.valuestr()[0] == '$') {
        /* if we got here, this is a field path expression */
        return ExpressionFieldPath::parse(expCtx, exprElement.str(), vps);
    } else if (type == Object) {
        return Expression::parseObject(expCtx, exprElement.Obj(), vps);
    } else if (type == Array) {
        return ExpressionArray::parse(expCtx, exprElement, vps);
    } else {
        return ExpressionConstant::parse(expCtx, exprElement, vps);
    }
}

namespace {
/**
 * UTF-8 multi-byte code points consist of one leading byte of the form 11xxxxxx, and potentially
 * many continuation bytes of the form 10xxxxxx. This method checks whether 'charByte' is a leading
 * byte.
 */
bool isLeadingByte(char charByte) {
    return (charByte & 0xc0) == 0xc0;
}

/**
 * UTF-8 single-byte code points are of the form 0xxxxxxx. This method checks whether 'charByte' is
 * a single-byte code point.
 */
bool isSingleByte(char charByte) {
    return (charByte & 0x80) == 0x0;
}

size_t getCodePointLength(char charByte) {
    if (isSingleByte(charByte)) {
        return 1;
    }

    invariant(isLeadingByte(charByte));

    // In UTF-8, the number of leading ones is the number of bytes the code point takes up.
    return countLeadingZeros64(~(uint64_t(charByte) << (64 - 8)));
}
}  // namespace

/* ------------------------- Register Date Expressions ----------------------------- */

REGISTER_EXPRESSION(dayOfMonth, ExpressionDayOfMonth::parse);
REGISTER_EXPRESSION(dayOfWeek, ExpressionDayOfWeek::parse);
REGISTER_EXPRESSION(dayOfYear, ExpressionDayOfYear::parse);
REGISTER_EXPRESSION(hour, ExpressionHour::parse);
REGISTER_EXPRESSION(isoDayOfWeek, ExpressionIsoDayOfWeek::parse);
REGISTER_EXPRESSION(isoWeek, ExpressionIsoWeek::parse);
REGISTER_EXPRESSION(isoWeekYear, ExpressionIsoWeekYear::parse);
REGISTER_EXPRESSION(millisecond, ExpressionMillisecond::parse);
REGISTER_EXPRESSION(minute, ExpressionMinute::parse);
REGISTER_EXPRESSION(month, ExpressionMonth::parse);
REGISTER_EXPRESSION(second, ExpressionSecond::parse);
REGISTER_EXPRESSION(week, ExpressionWeek::parse);
REGISTER_EXPRESSION(year, ExpressionYear::parse);

/* ----------------------- ExpressionAbs ---------------------------- */

Value ExpressionAbs::evaluateNumericArg(const Value& numericArg) const {
    BSONType type = numericArg.getType();
    if (type == NumberDouble) {
        return Value(std::abs(numericArg.getDouble()));
    } else if (type == NumberDecimal) {
        return Value(numericArg.getDecimal().toAbs());
    } else {
        long long num = numericArg.getLong();
        uassert(28680,
                "can't take $abs of long long min",
                num != std::numeric_limits<long long>::min());
        long long absVal = std::abs(num);
        return type == NumberLong ? Value(absVal) : Value::createIntOrLong(absVal);
    }
}

REGISTER_EXPRESSION(abs, ExpressionAbs::parse);
const char* ExpressionAbs::getOpName() const {
    return "$abs";
}

/* ------------------------- ExpressionAdd ----------------------------- */

Value ExpressionAdd::evaluate(const Document& root, Variables* variables) const {
    // We'll try to return the narrowest possible result value while avoiding overflow, loss
    // of precision due to intermediate rounding or implicit use of decimal types. To do that,
    // compute a compensated sum for non-decimal values and a separate decimal sum for decimal
    // values, and track the current narrowest type.
    DoubleDoubleSummation nonDecimalTotal;
    Decimal128 decimalTotal;
    BSONType totalType = NumberInt;
    bool haveDate = false;

    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; ++i) {
        Value val = vpOperand[i]->evaluate(root, variables);

        switch (val.getType()) {
            case NumberDecimal:
                decimalTotal = decimalTotal.add(val.getDecimal());
                totalType = NumberDecimal;
                break;
            case NumberDouble:
                nonDecimalTotal.addDouble(val.getDouble());
                if (totalType != NumberDecimal)
                    totalType = NumberDouble;
                break;
            case NumberLong:
                nonDecimalTotal.addLong(val.getLong());
                if (totalType == NumberInt)
                    totalType = NumberLong;
                break;
            case NumberInt:
                nonDecimalTotal.addDouble(val.getInt());
                break;
            case Date:
                uassert(16612, "only one date allowed in an $add expression", !haveDate);
                haveDate = true;
                nonDecimalTotal.addLong(val.getDate().toMillisSinceEpoch());
                break;
            default:
                uassert(16554,
                        str::stream() << "$add only supports numeric or date types, not "
                                      << typeName(val.getType()),
                        val.nullish());
                return Value(BSONNULL);
        }
    }

    if (haveDate) {
        int64_t longTotal;
        if (totalType == NumberDecimal) {
            longTotal = decimalTotal.add(nonDecimalTotal.getDecimal()).toLong();
        } else {
            uassert(ErrorCodes::Overflow, "date overflow in $add", nonDecimalTotal.fitsLong());
            longTotal = nonDecimalTotal.getLong();
        }
        return Value(Date_t::fromMillisSinceEpoch(longTotal));
    }
    switch (totalType) {
        case NumberDecimal:
            return Value(decimalTotal.add(nonDecimalTotal.getDecimal()));
        case NumberLong:
            dassert(nonDecimalTotal.isInteger());
            if (nonDecimalTotal.fitsLong())
                return Value(nonDecimalTotal.getLong());
        // Fallthrough.
        case NumberInt:
            if (nonDecimalTotal.fitsLong())
                return Value::createIntOrLong(nonDecimalTotal.getLong());
        // Fallthrough.
        case NumberDouble:
            return Value(nonDecimalTotal.getDouble());
        default:
            massert(16417, "$add resulted in a non-numeric type", false);
    }
}

REGISTER_EXPRESSION(add, ExpressionAdd::parse);
const char* ExpressionAdd::getOpName() const {
    return "$add";
}

/* ------------------------- ExpressionAllElementsTrue -------------------------- */

Value ExpressionAllElementsTrue::evaluate(const Document& root, Variables* variables) const {
    const Value arr = vpOperand[0]->evaluate(root, variables);
    uassert(17040,
            str::stream() << getOpName() << "'s argument must be an array, but is "
                          << typeName(arr.getType()),
            arr.isArray());
    const vector<Value>& array = arr.getArray();
    for (vector<Value>::const_iterator it = array.begin(); it != array.end(); ++it) {
        if (!it->coerceToBool()) {
            return Value(false);
        }
    }
    return Value(true);
}

REGISTER_EXPRESSION(allElementsTrue, ExpressionAllElementsTrue::parse);
const char* ExpressionAllElementsTrue::getOpName() const {
    return "$allElementsTrue";
}

/* ------------------------- ExpressionAnd ----------------------------- */

intrusive_ptr<Expression> ExpressionAnd::optimize() {
    /* optimize the conjunction as much as possible */
    intrusive_ptr<Expression> pE(ExpressionNary::optimize());

    /* if the result isn't a conjunction, we can't do anything */
    ExpressionAnd* pAnd = dynamic_cast<ExpressionAnd*>(pE.get());
    if (!pAnd)
        return pE;

    /*
      Check the last argument on the result; if it's not constant (as
      promised by ExpressionNary::optimize(),) then there's nothing
      we can do.
    */
    const size_t n = pAnd->vpOperand.size();
    // ExpressionNary::optimize() generates an ExpressionConstant for {$and:[]}.
    verify(n > 0);
    intrusive_ptr<Expression> pLast(pAnd->vpOperand[n - 1]);
    const ExpressionConstant* pConst = dynamic_cast<ExpressionConstant*>(pLast.get());
    if (!pConst)
        return pE;

    /*
      Evaluate and coerce the last argument to a boolean.  If it's false,
      then we can replace this entire expression.
     */
    bool last = pConst->getValue().coerceToBool();
    if (!last) {
        intrusive_ptr<ExpressionConstant> pFinal(
            ExpressionConstant::create(getExpressionContext(), Value(false)));
        return pFinal;
    }

    /*
      If we got here, the final operand was true, so we don't need it
      anymore.  If there was only one other operand, we don't need the
      conjunction either.  Note we still need to keep the promise that
      the result will be a boolean.
     */
    if (n == 2) {
        intrusive_ptr<Expression> pFinal(
            ExpressionCoerceToBool::create(getExpressionContext(), pAnd->vpOperand[0]));
        return pFinal;
    }

    /*
      Remove the final "true" value, and return the new expression.

      CW TODO:
      Note that because of any implicit conversions, we may need to
      apply an implicit boolean conversion.
    */
    pAnd->vpOperand.resize(n - 1);
    return pE;
}

Value ExpressionAnd::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(vpOperand[i]->evaluate(root, variables));
        if (!pValue.coerceToBool())
            return Value(false);
    }

    return Value(true);
}

REGISTER_EXPRESSION(and, ExpressionAnd::parse);
const char* ExpressionAnd::getOpName() const {
    return "$and";
}

/* ------------------------- ExpressionAnyElementTrue -------------------------- */

Value ExpressionAnyElementTrue::evaluate(const Document& root, Variables* variables) const {
    const Value arr = vpOperand[0]->evaluate(root, variables);
    uassert(17041,
            str::stream() << getOpName() << "'s argument must be an array, but is "
                          << typeName(arr.getType()),
            arr.isArray());
    const vector<Value>& array = arr.getArray();
    for (vector<Value>::const_iterator it = array.begin(); it != array.end(); ++it) {
        if (it->coerceToBool()) {
            return Value(true);
        }
    }
    return Value(false);
}

REGISTER_EXPRESSION(anyElementTrue, ExpressionAnyElementTrue::parse);
const char* ExpressionAnyElementTrue::getOpName() const {
    return "$anyElementTrue";
}

/* ---------------------- ExpressionArray --------------------------- */

Value ExpressionArray::evaluate(const Document& root, Variables* variables) const {
    vector<Value> values;
    values.reserve(vpOperand.size());
    for (auto&& expr : vpOperand) {
        Value elemVal = expr->evaluate(root, variables);
        values.push_back(elemVal.missing() ? Value(BSONNULL) : std::move(elemVal));
    }
    return Value(std::move(values));
}

Value ExpressionArray::serialize(bool explain) const {
    vector<Value> expressions;
    expressions.reserve(vpOperand.size());
    for (auto&& expr : vpOperand) {
        expressions.push_back(expr->serialize(explain));
    }
    return Value(std::move(expressions));
}

intrusive_ptr<Expression> ExpressionArray::optimize() {
    bool allValuesConstant = true;

    for (auto&& expr : vpOperand) {
        expr = expr->optimize();
        if (!dynamic_cast<ExpressionConstant*>(expr.get())) {
            allValuesConstant = false;
        }
    }

    // If all values in ExpressionArray are constant evaluate to ExpressionConstant.
    if (allValuesConstant) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables)));
    }
    return this;
}

const char* ExpressionArray::getOpName() const {
    // This should never be called, but is needed to inherit from ExpressionNary.
    return "$array";
}

/* ------------------------- ExpressionArrayElemAt -------------------------- */

Value ExpressionArrayElemAt::evaluate(const Document& root, Variables* variables) const {
    const Value array = vpOperand[0]->evaluate(root, variables);
    const Value indexArg = vpOperand[1]->evaluate(root, variables);

    if (array.nullish() || indexArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(28689,
            str::stream() << getOpName() << "'s first argument must be an array, but is "
                          << typeName(array.getType()),
            array.isArray());
    uassert(28690,
            str::stream() << getOpName() << "'s second argument must be a numeric value,"
                          << " but is "
                          << typeName(indexArg.getType()),
            indexArg.numeric());
    uassert(28691,
            str::stream() << getOpName() << "'s second argument must be representable as"
                          << " a 32-bit integer: "
                          << indexArg.coerceToDouble(),
            indexArg.integral());

    long long i = indexArg.coerceToLong();
    if (i < 0 && static_cast<size_t>(std::abs(i)) > array.getArrayLength()) {
        // Positive indices that are too large are handled automatically by Value.
        return Value();
    } else if (i < 0) {
        // Index from the back of the array.
        i = array.getArrayLength() + i;
    }
    const size_t index = static_cast<size_t>(i);
    return array[index];
}

REGISTER_EXPRESSION(arrayElemAt, ExpressionArrayElemAt::parse);
const char* ExpressionArrayElemAt::getOpName() const {
    return "$arrayElemAt";
}

/* ------------------------- ExpressionObjectToArray -------------------------- */


Value ExpressionObjectToArray::evaluate(const Document& root, Variables* variables) const {
    const Value targetVal = vpOperand[0]->evaluate(root, variables);

    if (targetVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40390,
            str::stream() << "$objectToArray requires a document input, found: "
                          << typeName(targetVal.getType()),
            (targetVal.getType() == BSONType::Object));

    vector<Value> output;

    FieldIterator iter = targetVal.getDocument().fieldIterator();
    while (iter.more()) {
        Document::FieldPair pair = iter.next();
        MutableDocument keyvalue;
        keyvalue.addField("k", Value(pair.first));
        keyvalue.addField("v", pair.second);
        output.push_back(keyvalue.freezeToValue());
    }

    return Value(output);
}

REGISTER_EXPRESSION(objectToArray, ExpressionObjectToArray::parse);
const char* ExpressionObjectToArray::getOpName() const {
    return "$objectToArray";
}

/* ------------------------- ExpressionArrayToObject -------------------------- */

Value ExpressionArrayToObject::evaluate(const Document& root, Variables* variables) const {
    const Value input = vpOperand[0]->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40386,
            str::stream() << "$arrayToObject requires an array input, found: "
                          << typeName(input.getType()),
            input.isArray());

    MutableDocument output;
    const vector<Value>& array = input.getArray();
    if (array.empty()) {
        return output.freezeToValue();
    }

    // There are two accepted input formats in an array: [ [key, val] ] or [ {k:key, v:val} ]. The
    // first array element determines the format for the rest of the array. Mixing input formats is
    // not allowed.
    bool inputArrayFormat;
    if (array[0].isArray()) {
        inputArrayFormat = true;
    } else if (array[0].getType() == BSONType::Object) {
        inputArrayFormat = false;
    } else {
        uasserted(40398,
                  str::stream() << "Unrecognised input type format for $arrayToObject: "
                                << typeName(array[0].getType()));
    }

    for (auto&& elem : array) {
        if (inputArrayFormat == true) {
            uassert(
                40396,
                str::stream() << "$arrayToObject requires a consistent input format. Elements must"
                                 "all be arrays or all be objects. Array was detected, now found: "
                              << typeName(elem.getType()),
                elem.isArray());

            const vector<Value>& valArray = elem.getArray();

            uassert(40397,
                    str::stream() << "$arrayToObject requires an array of size 2 arrays,"
                                     "found array of size: "
                                  << valArray.size(),
                    (valArray.size() == 2));

            uassert(40395,
                    str::stream() << "$arrayToObject requires an array of key-value pairs, where "
                                     "the key must be of type string. Found key type: "
                                  << typeName(valArray[0].getType()),
                    (valArray[0].getType() == BSONType::String));

            auto keyName = valArray[0].getStringData();

            uassert(4940400,
                    "Key field cannot contain an embedded null byte",
                    keyName.find('\0') == std::string::npos);

            output[keyName] = valArray[1];

        } else {
            uassert(
                40391,
                str::stream() << "$arrayToObject requires a consistent input format. Elements must"
                                 "all be arrays or all be objects. Object was detected, now found: "
                              << typeName(elem.getType()),
                (elem.getType() == BSONType::Object));

            uassert(40392,
                    str::stream() << "$arrayToObject requires an object keys of 'k' and 'v'. "
                                     "Found incorrect number of keys:"
                                  << elem.getDocument().size(),
                    (elem.getDocument().size() == 2));

            Value key = elem.getDocument().getField("k");
            Value value = elem.getDocument().getField("v");

            uassert(40393,
                    str::stream() << "$arrayToObject requires an object with keys 'k' and 'v'. "
                                     "Missing either or both keys from: "
                                  << elem.toString(),
                    (!key.missing() && !value.missing()));

            uassert(
                40394,
                str::stream() << "$arrayToObject requires an object with keys 'k' and 'v', where "
                                 "the value of 'k' must be of type string. Found type: "
                              << typeName(key.getType()),
                (key.getType() == BSONType::String));

            auto keyName = key.getStringData();

            uassert(4940401,
                    "Key field cannot contain an embedded null byte",
                    keyName.find('\0') == std::string::npos);

            output[keyName] = value;
        }
    }

    return output.freezeToValue();
}

REGISTER_EXPRESSION(arrayToObject, ExpressionArrayToObject::parse);
const char* ExpressionArrayToObject::getOpName() const {
    return "$arrayToObject";
}

/* ------------------------- ExpressionCeil -------------------------- */

Value ExpressionCeil::evaluateNumericArg(const Value& numericArg) const {
    // There's no point in taking the ceiling of integers or longs, it will have no effect.
    switch (numericArg.getType()) {
        case NumberDouble:
            return Value(std::ceil(numericArg.getDouble()));
        case NumberDecimal:
            // Round toward the nearest decimal with a zero exponent in the positive direction.
            return Value(numericArg.getDecimal().quantize(Decimal128::kNormalizedZero,
                                                          Decimal128::kRoundTowardPositive));
        default:
            return numericArg;
    }
}

REGISTER_EXPRESSION(ceil, ExpressionCeil::parse);
const char* ExpressionCeil::getOpName() const {
    return "$ceil";
}

/* -------------------- ExpressionCoerceToBool ------------------------- */

intrusive_ptr<ExpressionCoerceToBool> ExpressionCoerceToBool::create(
    const intrusive_ptr<ExpressionContext>& expCtx, const intrusive_ptr<Expression>& pExpression) {
    intrusive_ptr<ExpressionCoerceToBool> pNew(new ExpressionCoerceToBool(expCtx, pExpression));
    return pNew;
}

ExpressionCoerceToBool::ExpressionCoerceToBool(const intrusive_ptr<ExpressionContext>& expCtx,
                                               const intrusive_ptr<Expression>& pTheExpression)
    : Expression(expCtx), pExpression(pTheExpression) {}

intrusive_ptr<Expression> ExpressionCoerceToBool::optimize() {
    /* optimize the operand */
    pExpression = pExpression->optimize();

    /* if the operand already produces a boolean, then we don't need this */
    /* LATER - Expression to support a "typeof" query? */
    Expression* pE = pExpression.get();
    if (dynamic_cast<ExpressionAnd*>(pE) || dynamic_cast<ExpressionOr*>(pE) ||
        dynamic_cast<ExpressionNot*>(pE) || dynamic_cast<ExpressionCoerceToBool*>(pE))
        return pExpression;

    return intrusive_ptr<Expression>(this);
}

void ExpressionCoerceToBool::_doAddDependencies(DepsTracker* deps) const {
    pExpression->addDependencies(deps);
}

Value ExpressionCoerceToBool::evaluate(const Document& root, Variables* variables) const {
    Value pResult(pExpression->evaluate(root, variables));
    bool b = pResult.coerceToBool();
    if (b)
        return Value(true);
    return Value(false);
}

Value ExpressionCoerceToBool::serialize(bool explain) const {
    // When not explaining, serialize to an $and expression. When parsed, the $and expression
    // will be optimized back into a ExpressionCoerceToBool.
    const char* name = explain ? "$coerceToBool" : "$and";
    return Value(DOC(name << DOC_ARRAY(pExpression->serialize(explain))));
}

/* ----------------------- ExpressionCompare --------------------------- */

namespace {
struct BoundOp {
    ExpressionCompare::CmpOp op;

    auto operator()(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    BSONElement bsonExpr,
                    const VariablesParseState& vps) const {
        return ExpressionCompare::parse(expCtx, std::move(bsonExpr), vps, op);
    }
};
}  // namespace

REGISTER_EXPRESSION(cmp, BoundOp{ExpressionCompare::CMP});
REGISTER_EXPRESSION(eq, BoundOp{ExpressionCompare::EQ});
REGISTER_EXPRESSION(gt, BoundOp{ExpressionCompare::GT});
REGISTER_EXPRESSION(gte, BoundOp{ExpressionCompare::GTE});
REGISTER_EXPRESSION(lt, BoundOp{ExpressionCompare::LT});
REGISTER_EXPRESSION(lte, BoundOp{ExpressionCompare::LTE});
REGISTER_EXPRESSION(ne, BoundOp{ExpressionCompare::NE});

intrusive_ptr<Expression> ExpressionCompare::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement bsonExpr,
    const VariablesParseState& vps,
    CmpOp op) {
    intrusive_ptr<ExpressionCompare> expr = new ExpressionCompare(expCtx, op);
    ExpressionVector args = parseArguments(expCtx, bsonExpr, vps);
    expr->validateArguments(args);
    expr->vpOperand = args;
    return expr;
}

boost::intrusive_ptr<ExpressionCompare> ExpressionCompare::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    CmpOp cmpOp,
    const boost::intrusive_ptr<Expression>& exprLeft,
    const boost::intrusive_ptr<Expression>& exprRight) {
    boost::intrusive_ptr<ExpressionCompare> expr = new ExpressionCompare(expCtx, cmpOp);
    expr->vpOperand = {exprLeft, exprRight};
    return expr;
}

namespace {
// Lookup table for truth value returns
struct CmpLookup {
    const bool truthValue[3];                // truth value for -1, 0, 1
    const ExpressionCompare::CmpOp reverse;  // reverse(b,a) returns the same as op(a,b)
    const char name[5];                      // string name with trailing '\0'
};
static const CmpLookup cmpLookup[7] = {
    /*             -1      0      1      reverse                  name   */
    /* EQ  */ {{false, true, false}, ExpressionCompare::EQ, "$eq"},
    /* NE  */ {{true, false, true}, ExpressionCompare::NE, "$ne"},
    /* GT  */ {{false, false, true}, ExpressionCompare::LT, "$gt"},
    /* GTE */ {{false, true, true}, ExpressionCompare::LTE, "$gte"},
    /* LT  */ {{true, false, false}, ExpressionCompare::GT, "$lt"},
    /* LTE */ {{true, true, false}, ExpressionCompare::GTE, "$lte"},

    // CMP is special. Only name is used.
    /* CMP */ {{false, false, false}, ExpressionCompare::CMP, "$cmp"},
};
}


Value ExpressionCompare::evaluate(const Document& root, Variables* variables) const {
    Value pLeft(vpOperand[0]->evaluate(root, variables));
    Value pRight(vpOperand[1]->evaluate(root, variables));

    int cmp = getExpressionContext()->getValueComparator().compare(pLeft, pRight);

    // Make cmp one of 1, 0, or -1.
    if (cmp == 0) {
        // leave as 0
    } else if (cmp < 0) {
        cmp = -1;
    } else if (cmp > 0) {
        cmp = 1;
    }

    if (cmpOp == CMP)
        return Value(cmp);

    bool returnValue = cmpLookup[cmpOp].truthValue[cmp + 1];
    return Value(returnValue);
}

const char* ExpressionCompare::getOpName() const {
    return cmpLookup[cmpOp].name;
}

/* ------------------------- ExpressionConcat ----------------------------- */

Value ExpressionConcat::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();

    StringBuilder result;
    for (size_t i = 0; i < n; ++i) {
        Value val = vpOperand[i]->evaluate(root, variables);
        if (val.nullish())
            return Value(BSONNULL);

        uassert(16702,
                str::stream() << "$concat only supports strings, not " << typeName(val.getType()),
                val.getType() == String);

        result << val.coerceToString();
    }

    return Value(result.str());
}

REGISTER_EXPRESSION(concat, ExpressionConcat::parse);
const char* ExpressionConcat::getOpName() const {
    return "$concat";
}

/* ------------------------- ExpressionConcatArrays ----------------------------- */

Value ExpressionConcatArrays::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();
    vector<Value> values;

    for (size_t i = 0; i < n; ++i) {
        Value val = vpOperand[i]->evaluate(root, variables);
        if (val.nullish()) {
            return Value(BSONNULL);
        }

        uassert(28664,
                str::stream() << "$concatArrays only supports arrays, not "
                              << typeName(val.getType()),
                val.isArray());

        const auto& subValues = val.getArray();
        values.insert(values.end(), subValues.begin(), subValues.end());
    }
    return Value(std::move(values));
}

REGISTER_EXPRESSION(concatArrays, ExpressionConcatArrays::parse);
const char* ExpressionConcatArrays::getOpName() const {
    return "$concatArrays";
}

/* ----------------------- ExpressionCond ------------------------------ */

Value ExpressionCond::evaluate(const Document& root, Variables* variables) const {
    Value pCond(vpOperand[0]->evaluate(root, variables));
    int idx = pCond.coerceToBool() ? 1 : 2;
    return vpOperand[idx]->evaluate(root, variables);
}

intrusive_ptr<Expression> ExpressionCond::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    if (expr.type() != Object) {
        return Base::parse(expCtx, expr, vps);
    }
    verify(str::equals(expr.fieldName(), "$cond"));

    intrusive_ptr<ExpressionCond> ret = new ExpressionCond(expCtx);
    ret->vpOperand.resize(3);

    const BSONObj args = expr.embeddedObject();
    BSONForEach(arg, args) {
        if (str::equals(arg.fieldName(), "if")) {
            ret->vpOperand[0] = parseOperand(expCtx, arg, vps);
        } else if (str::equals(arg.fieldName(), "then")) {
            ret->vpOperand[1] = parseOperand(expCtx, arg, vps);
        } else if (str::equals(arg.fieldName(), "else")) {
            ret->vpOperand[2] = parseOperand(expCtx, arg, vps);
        } else {
            uasserted(17083,
                      str::stream() << "Unrecognized parameter to $cond: " << arg.fieldName());
        }
    }

    uassert(17080, "Missing 'if' parameter to $cond", ret->vpOperand[0]);
    uassert(17081, "Missing 'then' parameter to $cond", ret->vpOperand[1]);
    uassert(17082, "Missing 'else' parameter to $cond", ret->vpOperand[2]);

    return ret;
}

REGISTER_EXPRESSION(cond, ExpressionCond::parse);
const char* ExpressionCond::getOpName() const {
    return "$cond";
}

/* ---------------------- ExpressionConstant --------------------------- */

intrusive_ptr<Expression> ExpressionConstant::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement exprElement,
    const VariablesParseState& vps) {
    return new ExpressionConstant(expCtx, Value(exprElement));
}


intrusive_ptr<ExpressionConstant> ExpressionConstant::create(
    const intrusive_ptr<ExpressionContext>& expCtx, const Value& value) {
    intrusive_ptr<ExpressionConstant> pEC(new ExpressionConstant(expCtx, value));
    return pEC;
}

ExpressionConstant::ExpressionConstant(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const Value& value)
    : Expression(expCtx), _value(value) {}


intrusive_ptr<Expression> ExpressionConstant::optimize() {
    /* nothing to do */
    return intrusive_ptr<Expression>(this);
}

void ExpressionConstant::_doAddDependencies(DepsTracker* deps) const {
    /* nothing to do */
}

Value ExpressionConstant::evaluate(const Document& root, Variables* variables) const {
    return _value;
}

Value ExpressionConstant::serialize(bool explain) const {
    return serializeConstant(_value);
}

REGISTER_EXPRESSION(const, ExpressionConstant::parse);
REGISTER_EXPRESSION(literal, ExpressionConstant::parse);  // alias
const char* ExpressionConstant::getOpName() const {
    return "$const";
}

/* ---------------------- ExpressionDateFromParts ----------------------- */

/* Helper functions also shared with ExpressionDateToParts */

namespace {

boost::optional<TimeZone> makeTimeZone(const TimeZoneDatabase* tzdb,
                                       const Document& root,
                                       const Expression* timeZone,
                                       Variables* variables) {
    invariant(tzdb);

    if (!timeZone) {
        return mongo::TimeZoneDatabase::utcZone();
    }

    auto timeZoneId = timeZone->evaluate(root, variables);

    if (timeZoneId.nullish()) {
        return boost::none;
    }

    uassert(40517,
            str::stream() << "timezone must evaluate to a string, found "
                          << typeName(timeZoneId.getType()),
            timeZoneId.getType() == BSONType::String);

    return tzdb->getTimeZone(timeZoneId.getString());
}

}  // namespace

constexpr long long ExpressionDateFromParts::kMaxValueForDatePart;
constexpr long long ExpressionDateFromParts::kMinValueForDatePart;

REGISTER_EXPRESSION(dateFromParts, ExpressionDateFromParts::parse);
intrusive_ptr<Expression> ExpressionDateFromParts::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(40519,
            "$dateFromParts only supports an object as its argument",
            expr.type() == BSONType::Object);

    BSONElement yearElem;
    BSONElement monthElem;
    BSONElement dayElem;
    BSONElement hourElem;
    BSONElement minuteElem;
    BSONElement secondElem;
    BSONElement millisecondElem;
    BSONElement isoWeekYearElem;
    BSONElement isoWeekElem;
    BSONElement isoDayOfWeekElem;
    BSONElement timeZoneElem;

    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        auto field = arg.fieldNameStringData();

        if (field == "year"_sd) {
            yearElem = arg;
        } else if (field == "month"_sd) {
            monthElem = arg;
        } else if (field == "day"_sd) {
            dayElem = arg;
        } else if (field == "hour"_sd) {
            hourElem = arg;
        } else if (field == "minute"_sd) {
            minuteElem = arg;
        } else if (field == "second"_sd) {
            secondElem = arg;
        } else if (field == "millisecond"_sd) {
            millisecondElem = arg;
        } else if (field == "isoWeekYear"_sd) {
            isoWeekYearElem = arg;
        } else if (field == "isoWeek"_sd) {
            isoWeekElem = arg;
        } else if (field == "isoDayOfWeek"_sd) {
            isoDayOfWeekElem = arg;
        } else if (field == "timezone"_sd) {
            timeZoneElem = arg;
        } else {
            uasserted(40518,
                      str::stream() << "Unrecognized argument to $dateFromParts: "
                                    << arg.fieldName());
        }
    }

    if (!yearElem && !isoWeekYearElem) {
        uasserted(40516, "$dateFromParts requires either 'year' or 'isoWeekYear' to be present");
    }

    if (yearElem && (isoWeekYearElem || isoWeekElem || isoDayOfWeekElem)) {
        uasserted(40489, "$dateFromParts does not allow mixing natural dates with ISO dates");
    }

    if (isoWeekYearElem && (yearElem || monthElem || dayElem)) {
        uasserted(40525, "$dateFromParts does not allow mixing ISO dates with natural dates");
    }

    return new ExpressionDateFromParts(
        expCtx,
        yearElem ? parseOperand(expCtx, yearElem, vps) : nullptr,
        monthElem ? parseOperand(expCtx, monthElem, vps) : nullptr,
        dayElem ? parseOperand(expCtx, dayElem, vps) : nullptr,
        hourElem ? parseOperand(expCtx, hourElem, vps) : nullptr,
        minuteElem ? parseOperand(expCtx, minuteElem, vps) : nullptr,
        secondElem ? parseOperand(expCtx, secondElem, vps) : nullptr,
        millisecondElem ? parseOperand(expCtx, millisecondElem, vps) : nullptr,
        isoWeekYearElem ? parseOperand(expCtx, isoWeekYearElem, vps) : nullptr,
        isoWeekElem ? parseOperand(expCtx, isoWeekElem, vps) : nullptr,
        isoDayOfWeekElem ? parseOperand(expCtx, isoDayOfWeekElem, vps) : nullptr,
        timeZoneElem ? parseOperand(expCtx, timeZoneElem, vps) : nullptr);
}

ExpressionDateFromParts::ExpressionDateFromParts(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    intrusive_ptr<Expression> year,
    intrusive_ptr<Expression> month,
    intrusive_ptr<Expression> day,
    intrusive_ptr<Expression> hour,
    intrusive_ptr<Expression> minute,
    intrusive_ptr<Expression> second,
    intrusive_ptr<Expression> millisecond,
    intrusive_ptr<Expression> isoWeekYear,
    intrusive_ptr<Expression> isoWeek,
    intrusive_ptr<Expression> isoDayOfWeek,
    intrusive_ptr<Expression> timeZone)
    : Expression(expCtx),
      _year(std::move(year)),
      _month(std::move(month)),
      _day(std::move(day)),
      _hour(std::move(hour)),
      _minute(std::move(minute)),
      _second(std::move(second)),
      _millisecond(std::move(millisecond)),
      _isoWeekYear(std::move(isoWeekYear)),
      _isoWeek(std::move(isoWeek)),
      _isoDayOfWeek(std::move(isoDayOfWeek)),
      _timeZone(std::move(timeZone)) {}

intrusive_ptr<Expression> ExpressionDateFromParts::optimize() {
    if (_year) {
        _year = _year->optimize();
    }
    if (_month) {
        _month = _month->optimize();
    }
    if (_day) {
        _day = _day->optimize();
    }
    if (_hour) {
        _hour = _hour->optimize();
    }
    if (_minute) {
        _minute = _minute->optimize();
    }
    if (_second) {
        _second = _second->optimize();
    }
    if (_millisecond) {
        _millisecond = _millisecond->optimize();
    }
    if (_isoWeekYear) {
        _isoWeekYear = _isoWeekYear->optimize();
    }
    if (_isoWeek) {
        _isoWeek = _isoWeek->optimize();
    }
    if (_isoDayOfWeek) {
        _isoDayOfWeek = _isoDayOfWeek->optimize();
    }
    if (_timeZone) {
        _timeZone = _timeZone->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_year,
                                               _month,
                                               _day,
                                               _hour,
                                               _minute,
                                               _second,
                                               _millisecond,
                                               _isoWeekYear,
                                               _isoWeek,
                                               _isoDayOfWeek,
                                               _timeZone})) {

        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }

    return this;
}

Value ExpressionDateFromParts::serialize(bool explain) const {
    return Value(Document{
        {"$dateFromParts",
         Document{{"year", _year ? _year->serialize(explain) : Value()},
                  {"month", _month ? _month->serialize(explain) : Value()},
                  {"day", _day ? _day->serialize(explain) : Value()},
                  {"hour", _hour ? _hour->serialize(explain) : Value()},
                  {"minute", _minute ? _minute->serialize(explain) : Value()},
                  {"second", _second ? _second->serialize(explain) : Value()},
                  {"millisecond", _millisecond ? _millisecond->serialize(explain) : Value()},
                  {"isoWeekYear", _isoWeekYear ? _isoWeekYear->serialize(explain) : Value()},
                  {"isoWeek", _isoWeek ? _isoWeek->serialize(explain) : Value()},
                  {"isoDayOfWeek", _isoDayOfWeek ? _isoDayOfWeek->serialize(explain) : Value()},
                  {"timezone", _timeZone ? _timeZone->serialize(explain) : Value()}}}});
}

bool ExpressionDateFromParts::evaluateNumberWithDefault(const Document& root,
                                                        const Expression* field,
                                                        StringData fieldName,
                                                        long long defaultValue,
                                                        long long* returnValue,
                                                        Variables* variables) const {
    if (!field) {
        *returnValue = defaultValue;
        return true;
    }

    auto fieldValue = field->evaluate(root, variables);

    if (fieldValue.nullish()) {
        return false;
    }

    uassert(40515,
            str::stream() << "'" << fieldName << "' must evaluate to an integer, found "
                          << typeName(fieldValue.getType())
                          << " with value "
                          << fieldValue.toString(),
            fieldValue.integral64Bit());

    *returnValue = fieldValue.coerceToLong();

    return true;
}

bool ExpressionDateFromParts::evaluateNumberWithDefaultAndBounds(const Document& root,
                                                                 const Expression* field,
                                                                 StringData fieldName,
                                                                 long long defaultValue,
                                                                 long long* returnValue,
                                                                 Variables* variables) const {
    bool result =
        evaluateNumberWithDefault(root, field, fieldName, defaultValue, returnValue, variables);

    uassert(31034,
            str::stream() << "'" << fieldName << "'"
                          << " must evaluate to a value in the range ["
                          << kMinValueForDatePart
                          << ", "
                          << kMaxValueForDatePart
                          << "]; value "
                          << *returnValue
                          << " is not in range",
            !result ||
                (*returnValue >= kMinValueForDatePart && *returnValue <= kMaxValueForDatePart));

    return result;
}

Value ExpressionDateFromParts::evaluate(const Document& root, Variables* variables) const {
    long long hour, minute, second, millisecond;

    if (!evaluateNumberWithDefaultAndBounds(root, _hour.get(), "hour"_sd, 0, &hour, variables) ||
        !evaluateNumberWithDefaultAndBounds(
            root, _minute.get(), "minute"_sd, 0, &minute, variables) ||
        !evaluateNumberWithDefault(root, _second.get(), "second"_sd, 0, &second, variables) ||
        !evaluateNumberWithDefault(
            root, _millisecond.get(), "millisecond"_sd, 0, &millisecond, variables)) {
        // One of the evaluated inputs in nullish.
        return Value(BSONNULL);
    }

    auto timeZone =
        makeTimeZone(getExpressionContext()->timeZoneDatabase, root, _timeZone.get(), variables);

    if (!timeZone) {
        return Value(BSONNULL);
    }

    if (_year) {
        long long year, month, day;

        if (!evaluateNumberWithDefault(root, _year.get(), "year"_sd, 1970, &year, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, _month.get(), "month"_sd, 1, &month, variables) ||
            !evaluateNumberWithDefaultAndBounds(root, _day.get(), "day"_sd, 1, &day, variables)) {
            // One of the evaluated inputs in nullish.
            return Value(BSONNULL);
        }

        uassert(40523,
                str::stream() << "'year' must evaluate to an integer in the range " << 0 << " to "
                              << 9999
                              << ", found "
                              << year,
                year >= 0 && year <= 9999);

        return Value(
            timeZone->createFromDateParts(year, month, day, hour, minute, second, millisecond));
    }

    if (_isoWeekYear) {
        long long isoWeekYear, isoWeek, isoDayOfWeek;

        if (!evaluateNumberWithDefault(
                root, _isoWeekYear.get(), "isoWeekYear"_sd, 1970, &isoWeekYear, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, _isoWeek.get(), "isoWeek"_sd, 1, &isoWeek, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, _isoDayOfWeek.get(), "isoDayOfWeek"_sd, 1, &isoDayOfWeek, variables)) {
            // One of the evaluated inputs in nullish.
            return Value(BSONNULL);
        }

        uassert(31095,
                str::stream() << "'isoWeekYear' must evaluate to an integer in the range " << 0
                              << " to "
                              << 9999
                              << ", found "
                              << isoWeekYear,
                isoWeekYear >= 0 && isoWeekYear <= 9999);

        return Value(timeZone->createFromIso8601DateParts(
            isoWeekYear, isoWeek, isoDayOfWeek, hour, minute, second, millisecond));
    }

    MONGO_UNREACHABLE;
}

void ExpressionDateFromParts::_doAddDependencies(DepsTracker* deps) const {
    if (_year) {
        _year->addDependencies(deps);
    }
    if (_month) {
        _month->addDependencies(deps);
    }
    if (_day) {
        _day->addDependencies(deps);
    }
    if (_hour) {
        _hour->addDependencies(deps);
    }
    if (_minute) {
        _minute->addDependencies(deps);
    }
    if (_second) {
        _second->addDependencies(deps);
    }
    if (_millisecond) {
        _millisecond->addDependencies(deps);
    }
    if (_isoWeekYear) {
        _isoWeekYear->addDependencies(deps);
    }
    if (_isoWeek) {
        _isoWeek->addDependencies(deps);
    }
    if (_isoDayOfWeek) {
        _isoDayOfWeek->addDependencies(deps);
    }
    if (_timeZone) {
        _timeZone->addDependencies(deps);
    }
}

/* ---------------------- ExpressionDateFromString --------------------- */

REGISTER_EXPRESSION(dateFromString, ExpressionDateFromString::parse);
intrusive_ptr<Expression> ExpressionDateFromString::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(40540,
            str::stream() << "$dateFromString only supports an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == BSONType::Object);

    BSONElement dateStringElem, timeZoneElem, formatElem, onNullElem, onErrorElem;

    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        auto field = arg.fieldNameStringData();

        if (field == "format"_sd) {
            formatElem = arg;
        } else if (field == "dateString"_sd) {
            dateStringElem = arg;
        } else if (field == "timezone"_sd) {
            timeZoneElem = arg;
        } else if (field == "onNull"_sd) {
            onNullElem = arg;
        } else if (field == "onError"_sd) {
            onErrorElem = arg;
        } else {
            uasserted(40541,
                      str::stream() << "Unrecognized argument to $dateFromString: "
                                    << arg.fieldName());
        }
    }

    // The 'format', 'onNull' and 'onError' options were introduced in 4.0, and should not be
    // allowed in contexts where the maximum feature version is <= 4.0.
    if (expCtx->maxFeatureCompatibilityVersion &&
        *expCtx->maxFeatureCompatibilityVersion <
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
        uassert(
            ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << "\"format\" option to $dateFromString is not allowed with the current "
                             "feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            !formatElem);
        uassert(
            ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << "\"onNull\" option to $dateFromString is not allowed with the current "
                             "feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            !onNullElem);
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                str::stream()
                    << "\"onError\" option to $dateFromString is not allowed with the current "
                       "feature compatibility version. See "
                    << feature_compatibility_version_documentation::kCompatibilityLink
                    << " for more information.",
                !onErrorElem);
    }

    uassert(40542, "Missing 'dateString' parameter to $dateFromString", dateStringElem);

    return new ExpressionDateFromString(
        expCtx,
        parseOperand(expCtx, dateStringElem, vps),
        timeZoneElem ? parseOperand(expCtx, timeZoneElem, vps) : nullptr,
        formatElem ? parseOperand(expCtx, formatElem, vps) : nullptr,
        onNullElem ? parseOperand(expCtx, onNullElem, vps) : nullptr,
        onErrorElem ? parseOperand(expCtx, onErrorElem, vps) : nullptr);
}

ExpressionDateFromString::ExpressionDateFromString(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    intrusive_ptr<Expression> dateString,
    intrusive_ptr<Expression> timeZone,
    intrusive_ptr<Expression> format,
    intrusive_ptr<Expression> onNull,
    intrusive_ptr<Expression> onError)
    : Expression(expCtx),
      _dateString(std::move(dateString)),
      _timeZone(std::move(timeZone)),
      _format(std::move(format)),
      _onNull(std::move(onNull)),
      _onError(std::move(onError)) {}

intrusive_ptr<Expression> ExpressionDateFromString::optimize() {
    _dateString = _dateString->optimize();
    if (_timeZone) {
        _timeZone = _timeZone->optimize();
    }

    if (_format) {
        _format = _format->optimize();
    }

    if (_onNull) {
        _onNull = _onNull->optimize();
    }

    if (_onError) {
        _onError = _onError->optimize();
    }

    if (ExpressionConstant::allNullOrConstant(
            {_dateString, _timeZone, _format, _onNull, _onError})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    return this;
}

Value ExpressionDateFromString::serialize(bool explain) const {
    return Value(
        Document{{"$dateFromString",
                  Document{{"dateString", _dateString->serialize(explain)},
                           {"timezone", _timeZone ? _timeZone->serialize(explain) : Value()},
                           {"format", _format ? _format->serialize(explain) : Value()},
                           {"onNull", _onNull ? _onNull->serialize(explain) : Value()},
                           {"onError", _onError ? _onError->serialize(explain) : Value()}}}});
}

Value ExpressionDateFromString::evaluate(const Document& root, Variables* variables) const {
    const Value dateString = _dateString->evaluate(root, variables);
    Value formatValue;

    // Eagerly validate the format parameter, ignoring if nullish since the input string nullish
    // behavior takes precedence.
    if (_format) {
        formatValue = _format->evaluate(root, variables);
        if (!formatValue.nullish()) {
            uassert(40684,
                    str::stream() << "$dateFromString requires that 'format' be a string, found: "
                                  << typeName(formatValue.getType())
                                  << " with value "
                                  << formatValue.toString(),
                    formatValue.getType() == BSONType::String);

            TimeZone::validateFromStringFormat(formatValue.getStringData());
        }
    }

    // Evaluate the timezone parameter before checking for nullish input, as this will throw an
    // exception for an invalid timezone string.
    auto timeZone =
        makeTimeZone(getExpressionContext()->timeZoneDatabase, root, _timeZone.get(), variables);

    // Behavior for nullish input takes precedence over other nullish elements.
    if (dateString.nullish()) {
        return _onNull ? _onNull->evaluate(root, variables) : Value(BSONNULL);
    }

    try {
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "$dateFromString requires that 'dateString' be a string, found: "
                              << typeName(dateString.getType())
                              << " with value "
                              << dateString.toString(),
                dateString.getType() == BSONType::String);

        const auto dateTimeString = dateString.getStringData();

        if (!timeZone) {
            return Value(BSONNULL);
        }

        if (_format) {
            if (formatValue.nullish()) {
                return Value(BSONNULL);
            }

            return Value(getExpressionContext()->timeZoneDatabase->fromString(
                dateTimeString, timeZone.get(), formatValue.getStringData()));
        }

        return Value(
            getExpressionContext()->timeZoneDatabase->fromString(dateTimeString, timeZone.get()));
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (_onError) {
            return _onError->evaluate(root, variables);
        }
        throw;
    }
}

void ExpressionDateFromString::_doAddDependencies(DepsTracker* deps) const {
    _dateString->addDependencies(deps);
    if (_timeZone) {
        _timeZone->addDependencies(deps);
    }

    if (_format) {
        _format->addDependencies(deps);
    }

    if (_onNull) {
        _onNull->addDependencies(deps);
    }

    if (_onError) {
        _onError->addDependencies(deps);
    }
}

/* ---------------------- ExpressionDateToParts ----------------------- */

REGISTER_EXPRESSION(dateToParts, ExpressionDateToParts::parse);
intrusive_ptr<Expression> ExpressionDateToParts::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(40524,
            "$dateToParts only supports an object as its argument",
            expr.type() == BSONType::Object);

    BSONElement dateElem;
    BSONElement timeZoneElem;
    BSONElement isoDateElem;

    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        auto field = arg.fieldNameStringData();

        if (field == "date"_sd) {
            dateElem = arg;
        } else if (field == "timezone"_sd) {
            timeZoneElem = arg;
        } else if (field == "iso8601"_sd) {
            isoDateElem = arg;
        } else {
            uasserted(40520,
                      str::stream() << "Unrecognized argument to $dateToParts: "
                                    << arg.fieldName());
        }
    }

    uassert(40522, "Missing 'date' parameter to $dateToParts", dateElem);

    return new ExpressionDateToParts(
        expCtx,
        parseOperand(expCtx, dateElem, vps),
        timeZoneElem ? parseOperand(expCtx, timeZoneElem, vps) : nullptr,
        isoDateElem ? parseOperand(expCtx, isoDateElem, vps) : nullptr);
}

ExpressionDateToParts::ExpressionDateToParts(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             intrusive_ptr<Expression> date,
                                             intrusive_ptr<Expression> timeZone,
                                             intrusive_ptr<Expression> iso8601)
    : Expression(expCtx),
      _date(std::move(date)),
      _timeZone(std::move(timeZone)),
      _iso8601(std::move(iso8601)) {}

intrusive_ptr<Expression> ExpressionDateToParts::optimize() {
    _date = _date->optimize();
    if (_timeZone) {
        _timeZone = _timeZone->optimize();
    }
    if (_iso8601) {
        _iso8601 = _iso8601->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_date, _iso8601, _timeZone})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }

    return this;
}

Value ExpressionDateToParts::serialize(bool explain) const {
    return Value(
        Document{{"$dateToParts",
                  Document{{"date", _date->serialize(explain)},
                           {"timezone", _timeZone ? _timeZone->serialize(explain) : Value()},
                           {"iso8601", _iso8601 ? _iso8601->serialize(explain) : Value()}}}});
}

boost::optional<int> ExpressionDateToParts::evaluateIso8601Flag(const Document& root,
                                                                Variables* variables) const {
    if (!_iso8601) {
        return false;
    }

    auto iso8601Output = _iso8601->evaluate(root, variables);

    if (iso8601Output.nullish()) {
        return boost::none;
    }

    uassert(40521,
            str::stream() << "iso8601 must evaluate to a bool, found "
                          << typeName(iso8601Output.getType()),
            iso8601Output.getType() == BSONType::Bool);

    return iso8601Output.getBool();
}

Value ExpressionDateToParts::evaluate(const Document& root, Variables* variables) const {
    const Value date = _date->evaluate(root, variables);

    auto timeZone =
        makeTimeZone(getExpressionContext()->timeZoneDatabase, root, _timeZone.get(), variables);
    if (!timeZone) {
        return Value(BSONNULL);
    }

    auto iso8601 = evaluateIso8601Flag(root, variables);
    if (!iso8601) {
        return Value(BSONNULL);
    }

    if (date.nullish()) {
        return Value(BSONNULL);
    }

    auto dateValue = date.coerceToDate();

    if (*iso8601) {
        auto parts = timeZone->dateIso8601Parts(dateValue);
        return Value(Document{{"isoWeekYear", parts.year},
                              {"isoWeek", parts.weekOfYear},
                              {"isoDayOfWeek", parts.dayOfWeek},
                              {"hour", parts.hour},
                              {"minute", parts.minute},
                              {"second", parts.second},
                              {"millisecond", parts.millisecond}});
    } else {
        auto parts = timeZone->dateParts(dateValue);
        return Value(Document{{"year", parts.year},
                              {"month", parts.month},
                              {"day", parts.dayOfMonth},
                              {"hour", parts.hour},
                              {"minute", parts.minute},
                              {"second", parts.second},
                              {"millisecond", parts.millisecond}});
    }
}

void ExpressionDateToParts::_doAddDependencies(DepsTracker* deps) const {
    _date->addDependencies(deps);
    if (_timeZone) {
        _timeZone->addDependencies(deps);
    }
    if (_iso8601) {
        _iso8601->addDependencies(deps);
    }
}


/* ---------------------- ExpressionDateToString ----------------------- */

REGISTER_EXPRESSION(dateToString, ExpressionDateToString::parse);
intrusive_ptr<Expression> ExpressionDateToString::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    verify(str::equals(expr.fieldName(), "$dateToString"));

    uassert(18629,
            "$dateToString only supports an object as its argument",
            expr.type() == BSONType::Object);

    BSONElement formatElem, dateElem, timeZoneElem, onNullElem;
    for (auto&& arg : expr.embeddedObject()) {
        auto field = arg.fieldNameStringData();

        if (field == "format"_sd) {
            formatElem = arg;
        } else if (field == "date"_sd) {
            dateElem = arg;
        } else if (field == "timezone"_sd) {
            timeZoneElem = arg;
        } else if (field == "onNull"_sd) {
            onNullElem = arg;
        } else {
            uasserted(18534,
                      str::stream() << "Unrecognized argument to $dateToString: "
                                    << arg.fieldName());
        }
    }

    // The 'onNull' option was introduced in 4.0, and should not be allowed in contexts where the
    // maximum feature version is <= 4.0. Similarly, the 'format' option was made optional in 4.0,
    // so should be required in such scenarios.
    if (expCtx->maxFeatureCompatibilityVersion &&
        *expCtx->maxFeatureCompatibilityVersion <
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
        uassert(
            ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << "\"onNull\" option to $dateToString is not allowed with the current "
                             "feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            !onNullElem);

        uassert(ErrorCodes::QueryFeatureNotAllowed,
                str::stream() << "\"format\" option to $dateToString is required with the current "
                                 "feature compatibility version. See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << " for more information.",
                formatElem);
    }

    uassert(18628, "Missing 'date' parameter to $dateToString", !dateElem.eoo());

    return new ExpressionDateToString(expCtx,
                                      parseOperand(expCtx, dateElem, vps),
                                      formatElem ? parseOperand(expCtx, formatElem, vps) : nullptr,
                                      timeZoneElem ? parseOperand(expCtx, timeZoneElem, vps)
                                                   : nullptr,
                                      onNullElem ? parseOperand(expCtx, onNullElem, vps) : nullptr);
}

ExpressionDateToString::ExpressionDateToString(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    intrusive_ptr<Expression> date,
    intrusive_ptr<Expression> format,
    intrusive_ptr<Expression> timeZone,
    intrusive_ptr<Expression> onNull)
    : Expression(expCtx),
      _format(std::move(format)),
      _date(std::move(date)),
      _timeZone(std::move(timeZone)),
      _onNull(std::move(onNull)) {}

intrusive_ptr<Expression> ExpressionDateToString::optimize() {
    _date = _date->optimize();
    if (_timeZone) {
        _timeZone = _timeZone->optimize();
    }

    if (_onNull) {
        _onNull = _onNull->optimize();
    }

    if (_format) {
        _format = _format->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_date, _format, _timeZone, _onNull})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }

    return this;
}

Value ExpressionDateToString::serialize(bool explain) const {
    return Value(
        Document{{"$dateToString",
                  Document{{"date", _date->serialize(explain)},
                           {"format", _format ? _format->serialize(explain) : Value()},
                           {"timezone", _timeZone ? _timeZone->serialize(explain) : Value()},
                           {"onNull", _onNull ? _onNull->serialize(explain) : Value()}}}});
}

Value ExpressionDateToString::evaluate(const Document& root, Variables* variables) const {
    const Value date = _date->evaluate(root, variables);
    Value formatValue;

    // Eagerly validate the format parameter, ignoring if nullish since the input date nullish
    // behavior takes precedence.
    if (_format) {
        formatValue = _format->evaluate(root, variables);
        if (!formatValue.nullish()) {
            uassert(18533,
                    str::stream() << "$dateToString requires that 'format' be a string, found: "
                                  << typeName(formatValue.getType())
                                  << " with value "
                                  << formatValue.toString(),
                    formatValue.getType() == BSONType::String);

            TimeZone::validateToStringFormat(formatValue.getStringData());
        }
    }

    // Evaluate the timezone parameter before checking for nullish input, as this will throw an
    // exception for an invalid timezone string.
    auto timeZone =
        makeTimeZone(getExpressionContext()->timeZoneDatabase, root, _timeZone.get(), variables);

    if (date.nullish()) {
        return _onNull ? _onNull->evaluate(root, variables) : Value(BSONNULL);
    }

    if (!timeZone) {
        return Value(BSONNULL);
    }

    if (_format) {
        if (formatValue.nullish()) {
            return Value(BSONNULL);
        }

        return Value(uassertStatusOK(
            timeZone->formatDate(formatValue.getStringData(), date.coerceToDate())));
    }

    return Value(
        uassertStatusOK(timeZone->formatDate(Value::kISOFormatString, date.coerceToDate())));
}

void ExpressionDateToString::_doAddDependencies(DepsTracker* deps) const {
    _date->addDependencies(deps);
    if (_timeZone) {
        _timeZone->addDependencies(deps);
    }

    if (_onNull) {
        _onNull->addDependencies(deps);
    }

    if (_format) {
        _format->addDependencies(deps);
    }
}

/* ----------------------- ExpressionDivide ---------------------------- */

Value ExpressionDivide::evaluate(const Document& root, Variables* variables) const {
    Value lhs = vpOperand[0]->evaluate(root, variables);
    Value rhs = vpOperand[1]->evaluate(root, variables);

    auto assertNonZero = [](bool nonZero) { uassert(16608, "can't $divide by zero", nonZero); };

    if (lhs.numeric() && rhs.numeric()) {
        // If, and only if, either side is decimal, return decimal.
        if (lhs.getType() == NumberDecimal || rhs.getType() == NumberDecimal) {
            Decimal128 numer = lhs.coerceToDecimal();
            Decimal128 denom = rhs.coerceToDecimal();
            assertNonZero(!denom.isZero());
            return Value(numer.divide(denom));
        }

        double numer = lhs.coerceToDouble();
        double denom = rhs.coerceToDouble();
        assertNonZero(denom != 0.0);

        return Value(numer / denom);
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else {
        uasserted(16609,
                  str::stream() << "$divide only supports numeric types, not "
                                << typeName(lhs.getType())
                                << " and "
                                << typeName(rhs.getType()));
    }
}

REGISTER_EXPRESSION(divide, ExpressionDivide::parse);
const char* ExpressionDivide::getOpName() const {
    return "$divide";
}

/* ----------------------- ExpressionExp ---------------------------- */

Value ExpressionExp::evaluateNumericArg(const Value& numericArg) const {
    // $exp always returns either a double or a decimal number, as e is irrational.
    if (numericArg.getType() == NumberDecimal)
        return Value(numericArg.coerceToDecimal().exponential());

    return Value(exp(numericArg.coerceToDouble()));
}

REGISTER_EXPRESSION(exp, ExpressionExp::parse);
const char* ExpressionExp::getOpName() const {
    return "$exp";
}

/* ---------------------- ExpressionObject --------------------------- */

ExpressionObject::ExpressionObject(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   vector<pair<string, intrusive_ptr<Expression>>>&& expressions)
    : Expression(expCtx), _expressions(std::move(expressions)) {}

intrusive_ptr<ExpressionObject> ExpressionObject::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    vector<pair<string, intrusive_ptr<Expression>>>&& expressions) {
    return new ExpressionObject(expCtx, std::move(expressions));
}

intrusive_ptr<ExpressionObject> ExpressionObject::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONObj obj,
    const VariablesParseState& vps) {
    // Make sure we don't have any duplicate field names.
    stdx::unordered_set<string> specifiedFields;

    vector<pair<string, intrusive_ptr<Expression>>> expressions;
    for (auto&& elem : obj) {
        // Make sure this element has a valid field name. Use StringData here so that we can detect
        // if the field name contains a null byte.
        FieldPath::uassertValidFieldName(elem.fieldNameStringData());

        auto fieldName = elem.fieldName();
        uassert(16406,
                str::stream() << "duplicate field name specified in object literal: "
                              << obj.toString(),
                specifiedFields.find(fieldName) == specifiedFields.end());
        specifiedFields.insert(fieldName);
        expressions.emplace_back(fieldName, parseOperand(expCtx, elem, vps));
    }

    return new ExpressionObject{expCtx, std::move(expressions)};
}

intrusive_ptr<Expression> ExpressionObject::optimize() {
    bool allValuesConstant = true;
    for (auto&& pair : _expressions) {
        pair.second = pair.second->optimize();
        if (!dynamic_cast<ExpressionConstant*>(pair.second.get())) {
            allValuesConstant = false;
        }
    }
    // If all values in ExpressionObject are constant evaluate to ExpressionConstant.
    if (allValuesConstant) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables)));
    }
    return this;
}

void ExpressionObject::_doAddDependencies(DepsTracker* deps) const {
    for (auto&& pair : _expressions) {
        pair.second->addDependencies(deps);
    }
}

Value ExpressionObject::evaluate(const Document& root, Variables* variables) const {
    MutableDocument outputDoc;
    for (auto&& pair : _expressions) {
        outputDoc.addField(pair.first, pair.second->evaluate(root, variables));
    }
    return outputDoc.freezeToValue();
}

Value ExpressionObject::serialize(bool explain) const {
    MutableDocument outputDoc;
    for (auto&& pair : _expressions) {
        outputDoc.addField(pair.first, pair.second->serialize(explain));
    }
    return outputDoc.freezeToValue();
}

Expression::ComputedPaths ExpressionObject::getComputedPaths(const std::string& exprFieldPath,
                                                             Variables::Id renamingVar) const {
    ComputedPaths outputPaths;
    for (auto&& pair : _expressions) {
        auto exprComputedPaths = pair.second->getComputedPaths(pair.first, renamingVar);
        for (auto&& renames : exprComputedPaths.renames) {
            auto newPath = FieldPath::getFullyQualifiedPath(exprFieldPath, renames.first);
            outputPaths.renames[std::move(newPath)] = renames.second;
        }
        for (auto&& path : exprComputedPaths.paths) {
            outputPaths.paths.insert(FieldPath::getFullyQualifiedPath(exprFieldPath, path));
        }
    }

    return outputPaths;
}

/* --------------------- ExpressionFieldPath --------------------------- */

// this is the old deprecated version only used by tests not using variables
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const string& fieldPath) {
    return new ExpressionFieldPath(expCtx, "CURRENT." + fieldPath, Variables::kRootId);
}

// this is the new version that supports every syntax
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const string& raw,
    const VariablesParseState& vps) {
    uassert(16873,
            str::stream() << "FieldPath '" << raw << "' doesn't start with $",
            raw.c_str()[0] == '$');  // c_str()[0] is always a valid reference.

    uassert(16872,
            str::stream() << "'$' by itself is not a valid FieldPath",
            raw.size() >= 2);  // need at least "$" and either "$" or a field name

    if (raw[1] == '$') {
        const StringData rawSD = raw;
        const StringData fieldPath = rawSD.substr(2);  // strip off $$
        const StringData varName = fieldPath.substr(0, fieldPath.find('.'));
        Variables::uassertValidNameForUserRead(varName);
        return new ExpressionFieldPath(expCtx, fieldPath.toString(), vps.getVariable(varName));
    } else {
        return new ExpressionFieldPath(expCtx,
                                       "CURRENT." + raw.substr(1),  // strip the "$" prefix
                                       vps.getVariable("CURRENT"));
    }
}

ExpressionFieldPath::ExpressionFieldPath(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const string& theFieldPath,
                                         Variables::Id variable)
    : Expression(expCtx), _fieldPath(theFieldPath), _variable(variable) {}

intrusive_ptr<Expression> ExpressionFieldPath::optimize() {
    if (_variable == Variables::kRemoveId) {
        // The REMOVE system variable optimizes to a constant missing value.
        return ExpressionConstant::create(getExpressionContext(), Value());
    }

    if (getExpressionContext()->variables.hasConstantValue(_variable)) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables)));
    }

    return intrusive_ptr<Expression>(this);
}

void ExpressionFieldPath::_doAddDependencies(DepsTracker* deps) const {
    if (_variable == Variables::kRootId) {  // includes CURRENT when it is equivalent to ROOT.
        if (_fieldPath.getPathLength() == 1) {
            deps->needWholeDocument = true;  // need full doc if just "$$ROOT"
        } else {
            deps->fields.insert(_fieldPath.tail().fullPath());
        }
    } else if (Variables::isUserDefinedVariable(_variable)) {
        deps->vars.insert(_variable);
    }
}

Value ExpressionFieldPath::evaluatePathArray(size_t index, const Value& input) const {
    dassert(input.isArray());

    // Check for remaining path in each element of array
    vector<Value> result;
    const vector<Value>& array = input.getArray();
    for (size_t i = 0; i < array.size(); i++) {
        if (array[i].getType() != Object)
            continue;

        const Value nested = evaluatePath(index, array[i].getDocument());
        if (!nested.missing())
            result.push_back(nested);
    }

    return Value(std::move(result));
}
Value ExpressionFieldPath::evaluatePath(size_t index, const Document& input) const {
    // Note this function is very hot so it is important that is is well optimized.
    // In particular, all return paths should support RVO.

    /* if we've hit the end of the path, stop */
    if (index == _fieldPath.getPathLength() - 1)
        return input[_fieldPath.getFieldName(index)];

    // Try to dive deeper
    const Value val = input[_fieldPath.getFieldName(index)];
    switch (val.getType()) {
        case Object:
            return evaluatePath(index + 1, val.getDocument());

        case Array:
            return evaluatePathArray(index + 1, val);

        default:
            return Value();
    }
}

Value ExpressionFieldPath::evaluate(const Document& root, Variables* variables) const {
    if (_fieldPath.getPathLength() == 1)  // get the whole variable
        return variables->getValue(_variable, root);

    if (_variable == Variables::kRootId) {
        // ROOT is always a document so use optimized code path
        return evaluatePath(1, root);
    }

    Value var = variables->getValue(_variable, root);
    switch (var.getType()) {
        case Object:
            return evaluatePath(1, var.getDocument());
        case Array:
            return evaluatePathArray(1, var);
        default:
            return Value();
    }
}

Value ExpressionFieldPath::serialize(bool explain) const {
    if (_fieldPath.getFieldName(0) == "CURRENT" && _fieldPath.getPathLength() > 1) {
        // use short form for "$$CURRENT.foo" but not just "$$CURRENT"
        return Value("$" + _fieldPath.tail().fullPath());
    } else {
        return Value("$$" + _fieldPath.fullPath());
    }
}

Expression::ComputedPaths ExpressionFieldPath::getComputedPaths(const std::string& exprFieldPath,
                                                                Variables::Id renamingVar) const {
    // An expression field path is either considered a rename or a computed path. We need to find
    // out which case we fall into.
    //
    // The caller has told us that renames must have 'varId' as the first component. We also check
    // that there is only one additional component---no dotted field paths are allowed!  This is
    // because dotted ExpressionFieldPaths can actually reshape the document rather than just
    // changing the field names. This can happen only if there are arrays along the dotted path.
    //
    // For example, suppose you have document {a: [{b: 1}, {b: 2}]}. The projection {"c.d": "$a.b"}
    // does *not* perform the strict rename to yield document {c: [{d: 1}, {d: 2}]}. Instead, it
    // results in the document {c: {d: [1, 2]}}. Due to this reshaping, matches expressed over "a.b"
    // before the $project is applied may not have the same behavior when expressed over "c.d" after
    // the $project is applied.
    ComputedPaths outputPaths;
    if (_variable == renamingVar && _fieldPath.getPathLength() == 2u) {
        outputPaths.renames[exprFieldPath] = _fieldPath.tail().fullPath();
    } else {
        outputPaths.paths.insert(exprFieldPath);
    }

    return outputPaths;
}

/* ------------------------- ExpressionFilter ----------------------------- */

REGISTER_EXPRESSION(filter, ExpressionFilter::parse);
intrusive_ptr<Expression> ExpressionFilter::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vpsIn) {
    verify(str::equals(expr.fieldName(), "$filter"));

    uassert(28646, "$filter only supports an object as its argument", expr.type() == Object);

    // "cond" must be parsed after "as" regardless of BSON order.
    BSONElement inputElem;
    BSONElement asElem;
    BSONElement condElem;
    for (auto elem : expr.Obj()) {
        if (str::equals(elem.fieldName(), "input")) {
            inputElem = elem;
        } else if (str::equals(elem.fieldName(), "as")) {
            asElem = elem;
        } else if (str::equals(elem.fieldName(), "cond")) {
            condElem = elem;
        } else {
            uasserted(28647,
                      str::stream() << "Unrecognized parameter to $filter: " << elem.fieldName());
        }
    }

    uassert(28648, "Missing 'input' parameter to $filter", !inputElem.eoo());
    uassert(28650, "Missing 'cond' parameter to $filter", !condElem.eoo());

    // Parse "input", only has outer variables.
    intrusive_ptr<Expression> input = parseOperand(expCtx, inputElem, vpsIn);

    // Parse "as".
    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our variable, vpsIn doesn't.

    // If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    Variables::uassertValidNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // Parse "cond", has access to "as" variable.
    intrusive_ptr<Expression> cond = parseOperand(expCtx, condElem, vpsSub);

    return new ExpressionFilter(
        expCtx, std::move(varName), varId, std::move(input), std::move(cond));
}

ExpressionFilter::ExpressionFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   string varName,
                                   Variables::Id varId,
                                   intrusive_ptr<Expression> input,
                                   intrusive_ptr<Expression> filter)
    : Expression(expCtx),
      _varName(std::move(varName)),
      _varId(varId),
      _input(std::move(input)),
      _filter(std::move(filter)) {}

intrusive_ptr<Expression> ExpressionFilter::optimize() {
    // TODO handle when _input is constant.
    _input = _input->optimize();
    _filter = _filter->optimize();
    return this;
}

Value ExpressionFilter::serialize(bool explain) const {
    return Value(
        DOC("$filter" << DOC("input" << _input->serialize(explain) << "as" << _varName << "cond"
                                     << _filter->serialize(explain))));
}

Value ExpressionFilter::evaluate(const Document& root, Variables* variables) const {
    // We are guaranteed at parse time that this isn't using our _varId.
    const Value inputVal = _input->evaluate(root, variables);
    if (inputVal.nullish())
        return Value(BSONNULL);

    uassert(28651,
            str::stream() << "input to $filter must be an array not "
                          << typeName(inputVal.getType()),
            inputVal.isArray());

    const vector<Value>& input = inputVal.getArray();

    if (input.empty())
        return inputVal;

    vector<Value> output;
    for (const auto& elem : input) {
        variables->setValue(_varId, elem);

        if (_filter->evaluate(root, variables).coerceToBool()) {
            output.push_back(std::move(elem));
        }
    }

    return Value(std::move(output));
}

void ExpressionFilter::_doAddDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    _filter->addDependencies(deps);
}

/* ------------------------- ExpressionFloor -------------------------- */

Value ExpressionFloor::evaluateNumericArg(const Value& numericArg) const {
    // There's no point in taking the floor of integers or longs, it will have no effect.
    switch (numericArg.getType()) {
        case NumberDouble:
            return Value(std::floor(numericArg.getDouble()));
        case NumberDecimal:
            // Round toward the nearest decimal with a zero exponent in the negative direction.
            return Value(numericArg.getDecimal().quantize(Decimal128::kNormalizedZero,
                                                          Decimal128::kRoundTowardNegative));
        default:
            return numericArg;
    }
}

REGISTER_EXPRESSION(floor, ExpressionFloor::parse);
const char* ExpressionFloor::getOpName() const {
    return "$floor";
}

/* ------------------------- ExpressionLet ----------------------------- */

REGISTER_EXPRESSION(let, ExpressionLet::parse);
intrusive_ptr<Expression> ExpressionLet::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vpsIn) {
    verify(str::equals(expr.fieldName(), "$let"));

    uassert(16874, "$let only supports an object as its argument", expr.type() == Object);
    const BSONObj args = expr.embeddedObject();

    // varsElem must be parsed before inElem regardless of BSON order.
    BSONElement varsElem;
    BSONElement inElem;
    BSONForEach(arg, args) {
        if (str::equals(arg.fieldName(), "vars")) {
            varsElem = arg;
        } else if (str::equals(arg.fieldName(), "in")) {
            inElem = arg;
        } else {
            uasserted(16875,
                      str::stream() << "Unrecognized parameter to $let: " << arg.fieldName());
        }
    }

    uassert(16876, "Missing 'vars' parameter to $let", !varsElem.eoo());
    uassert(16877, "Missing 'in' parameter to $let", !inElem.eoo());

    // parse "vars"
    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our vars, vpsIn doesn't.
    VariableMap vars;
    BSONForEach(varElem, varsElem.embeddedObjectUserCheck()) {
        const string varName = varElem.fieldName();
        Variables::uassertValidNameForUserWrite(varName);
        Variables::Id id = vpsSub.defineVariable(varName);

        vars[id] = NameAndExpression(varName,
                                     parseOperand(expCtx, varElem, vpsIn));  // only has outer vars
    }

    // parse "in"
    intrusive_ptr<Expression> subExpression = parseOperand(expCtx, inElem, vpsSub);  // has our vars

    return new ExpressionLet(expCtx, vars, subExpression);
}

ExpressionLet::ExpressionLet(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             const VariableMap& vars,
                             intrusive_ptr<Expression> subExpression)
    : Expression(expCtx), _variables(vars), _subExpression(subExpression) {}

intrusive_ptr<Expression> ExpressionLet::optimize() {
    if (_variables.empty()) {
        // we aren't binding any variables so just return the subexpression
        return _subExpression->optimize();
    }

    for (VariableMap::iterator it = _variables.begin(), end = _variables.end(); it != end; ++it) {
        it->second.expression = it->second.expression->optimize();
    }

    _subExpression = _subExpression->optimize();

    return this;
}

Value ExpressionLet::serialize(bool explain) const {
    MutableDocument vars;
    for (VariableMap::const_iterator it = _variables.begin(), end = _variables.end(); it != end;
         ++it) {
        vars[it->second.name] = it->second.expression->serialize(explain);
    }

    return Value(
        DOC("$let" << DOC("vars" << vars.freeze() << "in" << _subExpression->serialize(explain))));
}

Value ExpressionLet::evaluate(const Document& root, Variables* variables) const {
    for (const auto& item : _variables) {
        // It is guaranteed at parse-time that these expressions don't use the variable ids we
        // are setting
        variables->setValue(item.first, item.second.expression->evaluate(root, variables));
    }

    return _subExpression->evaluate(root, variables);
}

void ExpressionLet::_doAddDependencies(DepsTracker* deps) const {
    for (auto&& idToNameExp : _variables) {
        // Add the external dependencies from the 'vars' statement.
        idToNameExp.second.expression->addDependencies(deps);
    }

    // Add subexpression dependencies, which may contain a mix of local and external variable refs.
    _subExpression->addDependencies(deps);
}

/* ------------------------- ExpressionMap ----------------------------- */

REGISTER_EXPRESSION(map, ExpressionMap::parse);
intrusive_ptr<Expression> ExpressionMap::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vpsIn) {
    verify(str::equals(expr.fieldName(), "$map"));

    uassert(16878, "$map only supports an object as its argument", expr.type() == Object);

    // "in" must be parsed after "as" regardless of BSON order
    BSONElement inputElem;
    BSONElement asElem;
    BSONElement inElem;
    const BSONObj args = expr.embeddedObject();
    BSONForEach(arg, args) {
        if (str::equals(arg.fieldName(), "input")) {
            inputElem = arg;
        } else if (str::equals(arg.fieldName(), "as")) {
            asElem = arg;
        } else if (str::equals(arg.fieldName(), "in")) {
            inElem = arg;
        } else {
            uasserted(16879,
                      str::stream() << "Unrecognized parameter to $map: " << arg.fieldName());
        }
    }

    uassert(16880, "Missing 'input' parameter to $map", !inputElem.eoo());
    uassert(16882, "Missing 'in' parameter to $map", !inElem.eoo());

    // parse "input"
    intrusive_ptr<Expression> input =
        parseOperand(expCtx, inputElem, vpsIn);  // only has outer vars

    // parse "as"
    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our vars, vpsIn doesn't.

    // If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    Variables::uassertValidNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // parse "in"
    intrusive_ptr<Expression> in =
        parseOperand(expCtx, inElem, vpsSub);  // has access to map variable

    return new ExpressionMap(expCtx, varName, varId, input, in);
}

ExpressionMap::ExpressionMap(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             const string& varName,
                             Variables::Id varId,
                             intrusive_ptr<Expression> input,
                             intrusive_ptr<Expression> each)
    : Expression(expCtx), _varName(varName), _varId(varId), _input(input), _each(each) {}

intrusive_ptr<Expression> ExpressionMap::optimize() {
    // TODO handle when _input is constant
    _input = _input->optimize();
    _each = _each->optimize();
    return this;
}

Value ExpressionMap::serialize(bool explain) const {
    return Value(DOC("$map" << DOC("input" << _input->serialize(explain) << "as" << _varName << "in"
                                           << _each->serialize(explain))));
}

Value ExpressionMap::evaluate(const Document& root, Variables* variables) const {
    // guaranteed at parse time that this isn't using our _varId
    const Value inputVal = _input->evaluate(root, variables);
    if (inputVal.nullish())
        return Value(BSONNULL);

    uassert(16883,
            str::stream() << "input to $map must be an array not " << typeName(inputVal.getType()),
            inputVal.isArray());

    const vector<Value>& input = inputVal.getArray();

    if (input.empty())
        return inputVal;

    vector<Value> output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        variables->setValue(_varId, input[i]);

        Value toInsert = _each->evaluate(root, variables);
        if (toInsert.missing())
            toInsert = Value(BSONNULL);  // can't insert missing values into array

        output.push_back(toInsert);
    }

    return Value(std::move(output));
}

void ExpressionMap::_doAddDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    _each->addDependencies(deps);
}

Expression::ComputedPaths ExpressionMap::getComputedPaths(const std::string& exprFieldPath,
                                                          Variables::Id renamingVar) const {
    auto inputFieldPath = dynamic_cast<ExpressionFieldPath*>(_input.get());
    if (!inputFieldPath) {
        return {{exprFieldPath}, {}};
    }

    auto inputComputedPaths = inputFieldPath->getComputedPaths("", renamingVar);
    if (inputComputedPaths.renames.empty()) {
        return {{exprFieldPath}, {}};
    }
    invariant(inputComputedPaths.renames.size() == 1u);
    auto fieldPathRenameIter = inputComputedPaths.renames.find("");
    invariant(fieldPathRenameIter != inputComputedPaths.renames.end());
    const auto& oldArrayName = fieldPathRenameIter->second;

    auto eachComputedPaths = _each->getComputedPaths(exprFieldPath, _varId);
    if (eachComputedPaths.renames.empty()) {
        return {{exprFieldPath}, {}};
    }

    // Append the name of the array to the beginning of the old field path.
    for (auto&& rename : eachComputedPaths.renames) {
        eachComputedPaths.renames[rename.first] =
            FieldPath::getFullyQualifiedPath(oldArrayName, rename.second);
    }
    return eachComputedPaths;
}

/* ------------------------- ExpressionMeta ----------------------------- */

REGISTER_EXPRESSION(meta, ExpressionMeta::parse);
intrusive_ptr<Expression> ExpressionMeta::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vpsIn) {
    uassert(17307, "$meta only supports string arguments", expr.type() == String);
    if (expr.valueStringData() == "textScore") {
        return new ExpressionMeta(expCtx, MetaType::TEXT_SCORE);
    } else if (expr.valueStringData() == "randVal") {
        return new ExpressionMeta(expCtx, MetaType::RAND_VAL);
    } else {
        uasserted(17308, "Unsupported argument to $meta: " + expr.String());
    }
}

ExpressionMeta::ExpressionMeta(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               MetaType metaType)
    : Expression(expCtx), _metaType(metaType) {}

Value ExpressionMeta::serialize(bool explain) const {
    switch (_metaType) {
        case MetaType::TEXT_SCORE:
            return Value(DOC("$meta"
                             << "textScore"_sd));
        case MetaType::RAND_VAL:
            return Value(DOC("$meta"
                             << "randVal"_sd));
    }
    MONGO_UNREACHABLE;
}

Value ExpressionMeta::evaluate(const Document& root, Variables* variables) const {
    switch (_metaType) {
        case MetaType::TEXT_SCORE:
            return root.hasTextScore() ? Value(root.getTextScore()) : Value();
        case MetaType::RAND_VAL:
            return root.hasRandMetaField() ? Value(root.getRandMetaField()) : Value();
    }
    MONGO_UNREACHABLE;
}

void ExpressionMeta::_doAddDependencies(DepsTracker* deps) const {
    if (_metaType == MetaType::TEXT_SCORE) {
        deps->setNeedTextScore(true);
    }
}

/* ----------------------- ExpressionMod ---------------------------- */

Value ExpressionMod::evaluate(const Document& root, Variables* variables) const {
    Value lhs = vpOperand[0]->evaluate(root, variables);
    Value rhs = vpOperand[1]->evaluate(root, variables);

    BSONType leftType = lhs.getType();
    BSONType rightType = rhs.getType();

    if (lhs.numeric() && rhs.numeric()) {
        auto assertNonZero = [](bool isZero) { uassert(16610, "can't $mod by zero", !isZero); };

        // If either side is decimal, perform the operation in decimal.
        if (leftType == NumberDecimal || rightType == NumberDecimal) {
            Decimal128 left = lhs.coerceToDecimal();
            Decimal128 right = rhs.coerceToDecimal();
            assertNonZero(right.isZero());

            return Value(left.modulo(right));
        }

        // ensure we aren't modding by 0
        double right = rhs.coerceToDouble();
        assertNonZero(right == 0);

        if (leftType == NumberDouble || (rightType == NumberDouble && !rhs.integral())) {
            // Need to do fmod. Integer-valued double case is handled below.

            double left = lhs.coerceToDouble();
            return Value(fmod(left, right));
        }

        if (leftType == NumberLong || rightType == NumberLong) {
            // if either is long, return long
            long long left = lhs.coerceToLong();
            long long rightLong = rhs.coerceToLong();
            return Value(mongoSafeMod(left, rightLong));
        }

        // lastly they must both be ints, return int
        int left = lhs.coerceToInt();
        int rightInt = rhs.coerceToInt();
        return Value(mongoSafeMod(left, rightInt));
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else {
        uasserted(16611,
                  str::stream() << "$mod only supports numeric types, not "
                                << typeName(lhs.getType())
                                << " and "
                                << typeName(rhs.getType()));
    }
}

REGISTER_EXPRESSION(mod, ExpressionMod::parse);
const char* ExpressionMod::getOpName() const {
    return "$mod";
}

/* ------------------------- ExpressionMultiply ----------------------------- */

Value ExpressionMultiply::evaluate(const Document& root, Variables* variables) const {
    /*
      We'll try to return the narrowest possible result value.  To do that
      without creating intermediate Values, do the arithmetic for double
      and integral types in parallel, tracking the current narrowest
      type.
     */
    double doubleProduct = 1;
    long long longProduct = 1;
    Decimal128 decimalProduct;  // This will be initialized on encountering the first decimal.

    BSONType productType = NumberInt;

    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; ++i) {
        Value val = vpOperand[i]->evaluate(root, variables);

        if (val.numeric()) {
            BSONType oldProductType = productType;
            productType = Value::getWidestNumeric(productType, val.getType());
            if (productType == NumberDecimal) {
                // On finding the first decimal, convert the partial product to decimal.
                if (oldProductType != NumberDecimal) {
                    decimalProduct = oldProductType == NumberDouble
                        ? Decimal128(doubleProduct, Decimal128::kRoundTo15Digits)
                        : Decimal128(static_cast<int64_t>(longProduct));
                }
                decimalProduct = decimalProduct.multiply(val.coerceToDecimal());
            } else {
                doubleProduct *= val.coerceToDouble();

                if (productType != NumberDouble) {
                    // If `productType` is not a double, it must be one of the integer types, so we
                    // attempt to update `longProduct`.
                    if (!std::isfinite(val.coerceToDouble()) ||
                        mongoSignedMultiplyOverflow64(
                            longProduct, val.coerceToLong(), &longProduct)) {
                        // The number is either Infinity or NaN, or the 'longProduct' would have
                        // overflowed, so we're abandoning it.
                        productType = NumberDouble;
                    }
                }
            }
        } else if (val.nullish()) {
            return Value(BSONNULL);
        } else {
            uasserted(16555,
                      str::stream() << "$multiply only supports numeric types, not "
                                    << typeName(val.getType()));
        }
    }

    if (productType == NumberDouble)
        return Value(doubleProduct);
    else if (productType == NumberLong)
        return Value(longProduct);
    else if (productType == NumberInt)
        return Value::createIntOrLong(longProduct);
    else if (productType == NumberDecimal)
        return Value(decimalProduct);
    else
        massert(16418, "$multiply resulted in a non-numeric type", false);
}

REGISTER_EXPRESSION(multiply, ExpressionMultiply::parse);
const char* ExpressionMultiply::getOpName() const {
    return "$multiply";
}

/* ----------------------- ExpressionIfNull ---------------------------- */

Value ExpressionIfNull::evaluate(const Document& root, Variables* variables) const {
    Value pLeft(vpOperand[0]->evaluate(root, variables));
    if (!pLeft.nullish())
        return pLeft;

    Value pRight(vpOperand[1]->evaluate(root, variables));
    return pRight;
}

REGISTER_EXPRESSION(ifNull, ExpressionIfNull::parse);
const char* ExpressionIfNull::getOpName() const {
    return "$ifNull";
}

/* ----------------------- ExpressionIn ---------------------------- */

Value ExpressionIn::evaluate(const Document& root, Variables* variables) const {
    Value argument(vpOperand[0]->evaluate(root, variables));
    Value arrayOfValues(vpOperand[1]->evaluate(root, variables));

    uassert(40081,
            str::stream() << "$in requires an array as a second argument, found: "
                          << typeName(arrayOfValues.getType()),
            arrayOfValues.isArray());
    for (auto&& value : arrayOfValues.getArray()) {
        if (getExpressionContext()->getValueComparator().evaluate(argument == value)) {
            return Value(true);
        }
    }
    return Value(false);
}

REGISTER_EXPRESSION(in, ExpressionIn::parse);
const char* ExpressionIn::getOpName() const {
    return "$in";
}

/* ----------------------- ExpressionIndexOfArray ------------------ */

namespace {

void uassertIfNotIntegralAndNonNegative(Value val,
                                        StringData expressionName,
                                        StringData argumentName) {
    uassert(40096,
            str::stream() << expressionName << "requires an integral " << argumentName
                          << ", found a value of type: "
                          << typeName(val.getType())
                          << ", with value: "
                          << val.toString(),
            val.integral());
    uassert(40097,
            str::stream() << expressionName << " requires a nonnegative " << argumentName
                          << ", found: "
                          << val.toString(),
            val.coerceToInt() >= 0);
}

}  // namespace

Value ExpressionIndexOfArray::evaluate(const Document& root, Variables* variables) const {
    Value arrayArg = vpOperand[0]->evaluate(root, variables);

    if (arrayArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40090,
            str::stream() << "$indexOfArray requires an array as a first argument, found: "
                          << typeName(arrayArg.getType()),
            arrayArg.isArray());

    std::vector<Value> array = arrayArg.getArray();
    auto args = evaluateAndValidateArguments(root, vpOperand, array.size(), variables);
    for (int i = args.startIndex; i < args.endIndex; i++) {
        if (getExpressionContext()->getValueComparator().evaluate(array[i] ==
                                                                  args.targetOfSearch)) {
            return Value(static_cast<int>(i));
        }
    }


    return Value(-1);
}

ExpressionIndexOfArray::Arguments ExpressionIndexOfArray::evaluateAndValidateArguments(
    const Document& root,
    const ExpressionVector& operands,
    size_t arrayLength,
    Variables* variables) const {

    int startIndex = 0;
    if (operands.size() > 2) {
        Value startIndexArg = operands[2]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(startIndexArg, getOpName(), "starting index");

        startIndex = startIndexArg.coerceToInt();
    }

    int endIndex = arrayLength;
    if (operands.size() > 3) {
        Value endIndexArg = operands[3]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(endIndexArg, getOpName(), "ending index");
        // Don't let 'endIndex' exceed the length of the array.

        endIndex = std::min(static_cast<int>(arrayLength), endIndexArg.coerceToInt());
    }
    return {vpOperand[1]->evaluate(root, variables), startIndex, endIndex};
}

/**
 * This class handles the case where IndexOfArray is given an ExpressionConstant
 * instead of using a vector and searching through it we can use a unordered_map
 * for O(1) lookup time.
 */
class ExpressionIndexOfArray::Optimized : public ExpressionIndexOfArray {
public:
    Optimized(const boost::intrusive_ptr<ExpressionContext>& expCtx,
              const ValueUnorderedMap<vector<int>>& indexMap,
              const ExpressionVector& operands)
        : ExpressionIndexOfArray(expCtx), _indexMap(std::move(indexMap)) {
        vpOperand = operands;
    }

    virtual Value evaluate(const Document& root, Variables* variables) const {

        auto args = evaluateAndValidateArguments(root, vpOperand, _indexMap.size(), variables);
        auto indexVec = _indexMap.find(args.targetOfSearch);

        if (indexVec == _indexMap.end())
            return Value(-1);

        // Search through the vector of indecies for first index in our range.
        for (auto index : indexVec->second) {
            if (index >= args.startIndex && index < args.endIndex) {
                return Value(index);
            }
        }
        // The value we are searching for exists but is not in our range.
        return Value(-1);
    }

private:
    // Maps the values in the array to the positions at which they occur. We need to remember the
    // positions so that we can verify they are in the appropriate range.
    const ValueUnorderedMap<vector<int>> _indexMap;
};

intrusive_ptr<Expression> ExpressionIndexOfArray::optimize() {
    // This will optimize all arguments to this expression.
    auto optimized = ExpressionNary::optimize();
    if (optimized.get() != this) {
        return optimized;
    }
    // If the input array is an ExpressionConstant we can optimize using a unordered_map instead of
    // an
    // array.
    if (auto constantArray = dynamic_cast<ExpressionConstant*>(vpOperand[0].get())) {
        const Value valueArray = constantArray->getValue();
        if (valueArray.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        uassert(50809,
                str::stream() << "First operand of $indexOfArray must be an array. First "
                              << "argument is of type: "
                              << typeName(valueArray.getType()),
                valueArray.isArray());

        auto arr = valueArray.getArray();

        // To handle the case of duplicate values the values need to map to a vector of indecies.
        auto indexMap =
            getExpressionContext()->getValueComparator().makeUnorderedValueMap<vector<int>>();

        for (int i = 0; i < int(arr.size()); i++) {
            if (indexMap.find(arr[i]) == indexMap.end()) {
                indexMap.emplace(arr[i], vector<int>());
            }
            indexMap[arr[i]].push_back(i);
        }
        return new Optimized(getExpressionContext(), indexMap, vpOperand);
    }
    return this;
}

REGISTER_EXPRESSION(indexOfArray, ExpressionIndexOfArray::parse);
const char* ExpressionIndexOfArray::getOpName() const {
    return "$indexOfArray";
}

/* ----------------------- ExpressionIndexOfBytes ------------------ */

namespace {

bool stringHasTokenAtIndex(size_t index, const std::string& input, const std::string& token) {
    if (token.size() + index > input.size()) {
        return false;
    }
    return input.compare(index, token.size(), token) == 0;
}

}  // namespace

Value ExpressionIndexOfBytes::evaluate(const Document& root, Variables* variables) const {
    Value stringArg = vpOperand[0]->evaluate(root, variables);

    if (stringArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40091,
            str::stream() << "$indexOfBytes requires a string as the first argument, found: "
                          << typeName(stringArg.getType()),
            stringArg.getType() == String);
    const std::string& input = stringArg.getString();

    Value tokenArg = vpOperand[1]->evaluate(root, variables);
    uassert(40092,
            str::stream() << "$indexOfBytes requires a string as the second argument, found: "
                          << typeName(tokenArg.getType()),
            tokenArg.getType() == String);
    const std::string& token = tokenArg.getString();

    size_t startIndex = 0;
    if (vpOperand.size() > 2) {
        Value startIndexArg = vpOperand[2]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(startIndexArg, getOpName(), "starting index");
        startIndex = static_cast<size_t>(startIndexArg.coerceToInt());
    }

    size_t endIndex = input.size();
    if (vpOperand.size() > 3) {
        Value endIndexArg = vpOperand[3]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(endIndexArg, getOpName(), "ending index");
        // Don't let 'endIndex' exceed the length of the string.
        endIndex = std::min(input.size(), static_cast<size_t>(endIndexArg.coerceToInt()));
    }

    if (startIndex > input.length() || endIndex < startIndex) {
        return Value(-1);
    }

    size_t position = input.substr(0, endIndex).find(token, startIndex);
    if (position == std::string::npos) {
        return Value(-1);
    }

    return Value(static_cast<int>(position));
}

REGISTER_EXPRESSION(indexOfBytes, ExpressionIndexOfBytes::parse);
const char* ExpressionIndexOfBytes::getOpName() const {
    return "$indexOfBytes";
}

/* ----------------------- ExpressionIndexOfCP --------------------- */

Value ExpressionIndexOfCP::evaluate(const Document& root, Variables* variables) const {
    Value stringArg = vpOperand[0]->evaluate(root, variables);

    if (stringArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40093,
            str::stream() << "$indexOfCP requires a string as the first argument, found: "
                          << typeName(stringArg.getType()),
            stringArg.getType() == String);
    const std::string& input = stringArg.getString();

    Value tokenArg = vpOperand[1]->evaluate(root, variables);
    uassert(40094,
            str::stream() << "$indexOfCP requires a string as the second argument, found: "
                          << typeName(tokenArg.getType()),
            tokenArg.getType() == String);
    const std::string& token = tokenArg.getString();

    size_t startCodePointIndex = 0;
    if (vpOperand.size() > 2) {
        Value startIndexArg = vpOperand[2]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(startIndexArg, getOpName(), "starting index");
        startCodePointIndex = static_cast<size_t>(startIndexArg.coerceToInt());
    }

    // Compute the length (in code points) of the input, and convert 'startCodePointIndex' to a byte
    // index.
    size_t codePointLength = 0;
    size_t startByteIndex = 0;
    for (size_t byteIx = 0; byteIx < input.size(); ++codePointLength) {
        if (codePointLength == startCodePointIndex) {
            // We have determined the byte at which our search will start.
            startByteIndex = byteIx;
        }

        uassert(40095,
                "$indexOfCP found bad UTF-8 in the input",
                !str::isUTF8ContinuationByte(input[byteIx]));
        byteIx += getCodePointLength(input[byteIx]);
    }

    size_t endCodePointIndex = codePointLength;
    if (vpOperand.size() > 3) {
        Value endIndexArg = vpOperand[3]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(endIndexArg, getOpName(), "ending index");

        // Don't let 'endCodePointIndex' exceed the number of code points in the string.
        endCodePointIndex =
            std::min(codePointLength, static_cast<size_t>(endIndexArg.coerceToInt()));
    }

    // If the start index is past the end, then always return -1 since 'token' does not exist within
    // these invalid bounds.
    if (endCodePointIndex < startCodePointIndex) {
        return Value(-1);
    }

    if (startByteIndex == 0 && input.empty() && token.empty()) {
        // If we are finding the index of "" in the string "", the below loop will not loop, so we
        // need a special case for this.
        return Value(0);
    }

    // We must keep track of which byte, and which code point, we are examining, being careful not
    // to overflow either the length of the string or the ending code point.

    size_t currentCodePointIndex = startCodePointIndex;
    for (size_t byteIx = startByteIndex; currentCodePointIndex < endCodePointIndex;
         ++currentCodePointIndex) {
        if (stringHasTokenAtIndex(byteIx, input, token)) {
            return Value(static_cast<int>(currentCodePointIndex));
        }
        byteIx += getCodePointLength(input[byteIx]);
    }

    return Value(-1);
}

REGISTER_EXPRESSION(indexOfCP, ExpressionIndexOfCP::parse);
const char* ExpressionIndexOfCP::getOpName() const {
    return "$indexOfCP";
}

/* ----------------------- ExpressionLn ---------------------------- */

Value ExpressionLn::evaluateNumericArg(const Value& numericArg) const {
    if (numericArg.getType() == NumberDecimal) {
        Decimal128 argDecimal = numericArg.getDecimal();
        if (argDecimal.isGreater(Decimal128::kNormalizedZero))
            return Value(argDecimal.logarithm());
        // Fall through for error case.
    }
    double argDouble = numericArg.coerceToDouble();
    uassert(28766,
            str::stream() << "$ln's argument must be a positive number, but is " << argDouble,
            argDouble > 0 || std::isnan(argDouble));
    return Value(std::log(argDouble));
}

REGISTER_EXPRESSION(ln, ExpressionLn::parse);
const char* ExpressionLn::getOpName() const {
    return "$ln";
}

/* ----------------------- ExpressionLog ---------------------------- */

Value ExpressionLog::evaluate(const Document& root, Variables* variables) const {
    Value argVal = vpOperand[0]->evaluate(root, variables);
    Value baseVal = vpOperand[1]->evaluate(root, variables);
    if (argVal.nullish() || baseVal.nullish())
        return Value(BSONNULL);

    uassert(28756,
            str::stream() << "$log's argument must be numeric, not " << typeName(argVal.getType()),
            argVal.numeric());
    uassert(28757,
            str::stream() << "$log's base must be numeric, not " << typeName(baseVal.getType()),
            baseVal.numeric());

    if (argVal.getType() == NumberDecimal || baseVal.getType() == NumberDecimal) {
        Decimal128 argDecimal = argVal.coerceToDecimal();
        Decimal128 baseDecimal = baseVal.coerceToDecimal();

        if (argDecimal.isGreater(Decimal128::kNormalizedZero) &&
            baseDecimal.isNotEqual(Decimal128(1)) &&
            baseDecimal.isGreater(Decimal128::kNormalizedZero)) {
            return Value(argDecimal.logarithm(baseDecimal));
        }
        // Fall through for error cases.
    }

    double argDouble = argVal.coerceToDouble();
    double baseDouble = baseVal.coerceToDouble();
    uassert(28758,
            str::stream() << "$log's argument must be a positive number, but is " << argDouble,
            argDouble > 0 || std::isnan(argDouble));
    uassert(28759,
            str::stream() << "$log's base must be a positive number not equal to 1, but is "
                          << baseDouble,
            (baseDouble > 0 && baseDouble != 1) || std::isnan(baseDouble));
    return Value(std::log(argDouble) / std::log(baseDouble));
}

REGISTER_EXPRESSION(log, ExpressionLog::parse);
const char* ExpressionLog::getOpName() const {
    return "$log";
}

/* ----------------------- ExpressionLog10 ---------------------------- */

Value ExpressionLog10::evaluateNumericArg(const Value& numericArg) const {
    if (numericArg.getType() == NumberDecimal) {
        Decimal128 argDecimal = numericArg.getDecimal();
        if (argDecimal.isGreater(Decimal128::kNormalizedZero))
            return Value(argDecimal.logarithm(Decimal128(10)));
        // Fall through for error case.
    }

    double argDouble = numericArg.coerceToDouble();
    uassert(28761,
            str::stream() << "$log10's argument must be a positive number, but is " << argDouble,
            argDouble > 0 || std::isnan(argDouble));
    return Value(std::log10(argDouble));
}

REGISTER_EXPRESSION(log10, ExpressionLog10::parse);
const char* ExpressionLog10::getOpName() const {
    return "$log10";
}

/* ------------------------ ExpressionNary ----------------------------- */

/**
 * Optimize a general Nary expression.
 *
 * The optimization has the following properties:
 *   1) Optimize each of the operators.
 *   2) If the operand is associative, flatten internal operators of the same type. I.e.:
 *      A+B+(C+D)+E => A+B+C+D+E
 *   3) If the operand is commutative & associative, group all constant operators. For example:
 *      c1 + c2 + n1 + c3 + n2 => n1 + n2 + c1 + c2 + c3
 *   4) If the operand is associative, execute the operation over all the contiguous constant
 *      operators and replacing them by the result. For example: c1 + c2 + n1 + c3 + c4 + n5 =>
 *      c5 = c1 + c2, c6 = c3 + c4 => c5 + n1 + c6 + n5
 *
 * It returns the optimized expression. It can be exactly the same expression, a modified version
 * of the same expression or a completely different expression.
 */
intrusive_ptr<Expression> ExpressionNary::optimize() {
    uint32_t constOperandCount = 0;

    for (auto& operand : vpOperand) {
        operand = operand->optimize();
        if (dynamic_cast<ExpressionConstant*>(operand.get())) {
            ++constOperandCount;
        }
    }
    // If all the operands are constant expressions, collapse the expression into one constant
    // expression.
    if (constOperandCount == vpOperand.size()) {
        return intrusive_ptr<Expression>(ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables))));
    }

    // If the expression is associative, we can collapse all the consecutive constant operands into
    // one by applying the expression to those consecutive constant operands.
    // If the expression is also commutative we can reorganize all the operands so that all of the
    // constant ones are together (arbitrarily at the back) and we can collapse all of them into
    // one.
    if (isAssociative()) {
        ExpressionVector constExpressions;
        ExpressionVector optimizedOperands;
        for (size_t i = 0; i < vpOperand.size();) {
            intrusive_ptr<Expression> operand = vpOperand[i];
            // If the operand is a constant one, add it to the current list of consecutive constant
            // operands.
            if (dynamic_cast<ExpressionConstant*>(operand.get())) {
                constExpressions.push_back(operand);
                ++i;
                continue;
            }

            // If the operand is exactly the same type as the one we are currently optimizing and
            // is also associative, replace the expression for the operands it has.
            // E.g: sum(a, b, sum(c, d), e) => sum(a, b, c, d, e)
            ExpressionNary* nary = dynamic_cast<ExpressionNary*>(operand.get());
            if (nary && str::equals(nary->getOpName(), getOpName()) && nary->isAssociative()) {
                invariant(!nary->vpOperand.empty());
                vpOperand[i] = std::move(nary->vpOperand[0]);
                vpOperand.insert(
                    vpOperand.begin() + i + 1, nary->vpOperand.begin() + 1, nary->vpOperand.end());
                continue;
            }

            // If the operand is not a constant nor a same-type expression and the expression is
            // not commutative, evaluate an expression of the same type as the one we are
            // optimizing on the list of consecutive constant operands and use the resulting value
            // as a constant expression operand.
            // If the list of consecutive constant operands has less than 2 operands just place
            // back the operands.
            if (!isCommutative()) {
                if (constExpressions.size() > 1) {
                    ExpressionVector vpOperandSave = std::move(vpOperand);
                    vpOperand = std::move(constExpressions);
                    optimizedOperands.emplace_back(ExpressionConstant::create(
                        getExpressionContext(),
                        evaluate(Document(), &getExpressionContext()->variables)));
                    vpOperand = std::move(vpOperandSave);
                } else {
                    optimizedOperands.insert(
                        optimizedOperands.end(), constExpressions.begin(), constExpressions.end());
                }
                constExpressions.clear();
            }
            optimizedOperands.push_back(operand);
            ++i;
        }

        if (constExpressions.size() > 1) {
            vpOperand = std::move(constExpressions);
            optimizedOperands.emplace_back(ExpressionConstant::create(
                getExpressionContext(),
                evaluate(Document(), &(getExpressionContext()->variables))));
        } else {
            optimizedOperands.insert(
                optimizedOperands.end(), constExpressions.begin(), constExpressions.end());
        }

        vpOperand = std::move(optimizedOperands);
    }
    return this;
}

void ExpressionNary::_doAddDependencies(DepsTracker* deps) const {
    for (auto&& operand : vpOperand) {
        operand->addDependencies(deps);
    }
}

void ExpressionNary::addOperand(const intrusive_ptr<Expression>& pExpression) {
    vpOperand.push_back(pExpression);
}

Value ExpressionNary::serialize(bool explain) const {
    const size_t nOperand = vpOperand.size();
    vector<Value> array;
    /* build up the array */
    for (size_t i = 0; i < nOperand; i++)
        array.push_back(vpOperand[i]->serialize(explain));

    return Value(DOC(getOpName() << array));
}

/* ------------------------- ExpressionNot ----------------------------- */

Value ExpressionNot::evaluate(const Document& root, Variables* variables) const {
    Value pOp(vpOperand[0]->evaluate(root, variables));

    bool b = pOp.coerceToBool();
    return Value(!b);
}

REGISTER_EXPRESSION(not, ExpressionNot::parse);
const char* ExpressionNot::getOpName() const {
    return "$not";
}

/* -------------------------- ExpressionOr ----------------------------- */

Value ExpressionOr::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(vpOperand[i]->evaluate(root, variables));
        if (pValue.coerceToBool())
            return Value(true);
    }

    return Value(false);
}

intrusive_ptr<Expression> ExpressionOr::optimize() {
    /* optimize the disjunction as much as possible */
    intrusive_ptr<Expression> pE(ExpressionNary::optimize());

    /* if the result isn't a disjunction, we can't do anything */
    ExpressionOr* pOr = dynamic_cast<ExpressionOr*>(pE.get());
    if (!pOr)
        return pE;

    /*
      Check the last argument on the result; if it's not constant (as
      promised by ExpressionNary::optimize(),) then there's nothing
      we can do.
    */
    const size_t n = pOr->vpOperand.size();
    // ExpressionNary::optimize() generates an ExpressionConstant for {$or:[]}.
    verify(n > 0);
    intrusive_ptr<Expression> pLast(pOr->vpOperand[n - 1]);
    const ExpressionConstant* pConst = dynamic_cast<ExpressionConstant*>(pLast.get());
    if (!pConst)
        return pE;

    /*
      Evaluate and coerce the last argument to a boolean.  If it's true,
      then we can replace this entire expression.
     */
    bool last = pConst->getValue().coerceToBool();
    if (last) {
        intrusive_ptr<ExpressionConstant> pFinal(
            ExpressionConstant::create(getExpressionContext(), Value(true)));
        return pFinal;
    }

    /*
      If we got here, the final operand was false, so we don't need it
      anymore.  If there was only one other operand, we don't need the
      conjunction either.  Note we still need to keep the promise that
      the result will be a boolean.
     */
    if (n == 2) {
        intrusive_ptr<Expression> pFinal(
            ExpressionCoerceToBool::create(getExpressionContext(), pOr->vpOperand[0]));
        return pFinal;
    }

    /*
      Remove the final "false" value, and return the new expression.
    */
    pOr->vpOperand.resize(n - 1);
    return pE;
}

REGISTER_EXPRESSION(or, ExpressionOr::parse);
const char* ExpressionOr::getOpName() const {
    return "$or";
}

namespace {
/**
 * Helper for ExpressionPow to determine wither base^exp can be represented in a 64 bit int.
 *
 *'base' and 'exp' are both integers. Assumes 'exp' is in the range [0, 63].
 */
bool representableAsLong(long long base, long long exp) {
    invariant(exp <= 63);
    invariant(exp >= 0);
    struct MinMax {
        long long min;
        long long max;
    };

    // Array indices correspond to exponents 0 through 63. The values in each index are the min
    // and max bases, respectively, that can be raised to that exponent without overflowing a
    // 64-bit int. For max bases, this was computed by solving for b in
    // b = (2^63-1)^(1/exp) for exp = [0, 63] and truncating b. To calculate min bases, for even
    // exps the equation  used was b = (2^63-1)^(1/exp), and for odd exps the equation used was
    // b = (-2^63)^(1/exp). Since the magnitude of long min is greater than long max, the
    // magnitude of some of the min bases raised to odd exps is greater than the corresponding
    // max bases raised to the same exponents.

    static const MinMax kBaseLimits[] = {
        {std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()},  // 0
        {std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()},
        {-3037000499LL, 3037000499LL},
        {-2097152, 2097151},
        {-55108, 55108},
        {-6208, 6208},
        {-1448, 1448},
        {-512, 511},
        {-234, 234},
        {-128, 127},
        {-78, 78},  // 10
        {-52, 52},
        {-38, 38},
        {-28, 28},
        {-22, 22},
        {-18, 18},
        {-15, 15},
        {-13, 13},
        {-11, 11},
        {-9, 9},
        {-8, 8},  // 20
        {-8, 7},
        {-7, 7},
        {-6, 6},
        {-6, 6},
        {-5, 5},
        {-5, 5},
        {-5, 5},
        {-4, 4},
        {-4, 4},
        {-4, 4},  // 30
        {-4, 4},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-2, 2},  // 40
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},  // 50
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},  // 60
        {-2, 2},
        {-2, 2},
        {-2, 1}};

    return base >= kBaseLimits[exp].min && base <= kBaseLimits[exp].max;
};
}

/* ----------------------- ExpressionPow ---------------------------- */

intrusive_ptr<Expression> ExpressionPow::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Value base, Value exp) {
    intrusive_ptr<ExpressionPow> expr(new ExpressionPow(expCtx));
    expr->vpOperand.push_back(
        ExpressionConstant::create(expr->getExpressionContext(), std::move(base)));
    expr->vpOperand.push_back(
        ExpressionConstant::create(expr->getExpressionContext(), std::move(exp)));
    return expr;
}

Value ExpressionPow::evaluate(const Document& root, Variables* variables) const {
    Value baseVal = vpOperand[0]->evaluate(root, variables);
    Value expVal = vpOperand[1]->evaluate(root, variables);
    if (baseVal.nullish() || expVal.nullish())
        return Value(BSONNULL);

    BSONType baseType = baseVal.getType();
    BSONType expType = expVal.getType();

    uassert(28762,
            str::stream() << "$pow's base must be numeric, not " << typeName(baseType),
            baseVal.numeric());
    uassert(28763,
            str::stream() << "$pow's exponent must be numeric, not " << typeName(expType),
            expVal.numeric());

    auto checkNonZeroAndNeg = [](bool isZeroAndNeg) {
        uassert(28764, "$pow cannot take a base of 0 and a negative exponent", !isZeroAndNeg);
    };

    // If either argument is decimal, return a decimal.
    if (baseType == NumberDecimal || expType == NumberDecimal) {
        Decimal128 baseDecimal = baseVal.coerceToDecimal();
        Decimal128 expDecimal = expVal.coerceToDecimal();
        checkNonZeroAndNeg(baseDecimal.isZero() && expDecimal.isNegative());
        return Value(baseDecimal.power(expDecimal));
    }

    // pow() will cast args to doubles.
    double baseDouble = baseVal.coerceToDouble();
    double expDouble = expVal.coerceToDouble();
    checkNonZeroAndNeg(baseDouble == 0 && expDouble < 0);

    // If either argument is a double, return a double.
    if (baseType == NumberDouble || expType == NumberDouble) {
        return Value(std::pow(baseDouble, expDouble));
    }

    // If either number is a long, return a long. If both numbers are ints, then return an int if
    // the result fits or a long if it is too big.
    const auto formatResult = [baseType, expType](long long res) {
        if (baseType == NumberLong || expType == NumberLong) {
            return Value(res);
        }
        return Value::createIntOrLong(res);
    };

    const long long baseLong = baseVal.getLong();
    const long long expLong = expVal.getLong();

    // Use this when the result cannot be represented as a long.
    const auto computeDoubleResult = [baseLong, expLong]() {
        return Value(std::pow(baseLong, expLong));
    };

    // Avoid doing repeated multiplication or using std::pow if the base is -1, 0, or 1.
    if (baseLong == 0) {
        if (expLong == 0) {
            // 0^0 = 1.
            return formatResult(1);
        } else if (expLong > 0) {
            // 0^x where x > 0 is 0.
            return formatResult(0);
        }

        // We should have checked earlier that 0 to a negative power is banned.
        MONGO_UNREACHABLE;
    } else if (baseLong == 1) {
        return formatResult(1);
    } else if (baseLong == -1) {
        // -1^0 = -1^2 = -1^4 = -1^6 ... = 1
        // -1^1 = -1^3 = -1^5 = -1^7 ... = -1
        return formatResult((expLong % 2 == 0) ? 1 : -1);
    } else if (expLong > 63 || expLong < 0) {
        // If the base is not 0, 1, or -1 and the exponent is too large, or negative,
        // the result cannot be represented as a long.
        return computeDoubleResult();
    }

    // It's still possible that the result cannot be represented as a long. If that's the case,
    // return a double.
    if (!representableAsLong(baseLong, expLong)) {
        return computeDoubleResult();
    }

    // Use repeated multiplication, since pow() casts args to doubles which could result in
    // loss of precision if arguments are very large.
    const auto computeWithRepeatedMultiplication = [](long long base, long long exp) {
        long long result = 1;

        while (exp > 1) {
            if (exp % 2 == 1) {
                result *= base;
                exp--;
            }
            // 'exp' is now guaranteed to be even.
            base *= base;
            exp /= 2;
        }

        if (exp) {
            invariant(exp == 1);
            result *= base;
        }

        return result;
    };

    return formatResult(computeWithRepeatedMultiplication(baseLong, expLong));
}

REGISTER_EXPRESSION(pow, ExpressionPow::parse);
const char* ExpressionPow::getOpName() const {
    return "$pow";
}

/* ------------------------- ExpressionRange ------------------------------ */

Value ExpressionRange::evaluate(const Document& root, Variables* variables) const {
    Value startVal(vpOperand[0]->evaluate(root, variables));
    Value endVal(vpOperand[1]->evaluate(root, variables));

    uassert(34443,
            str::stream() << "$range requires a numeric starting value, found value of type: "
                          << typeName(startVal.getType()),
            startVal.numeric());
    uassert(34444,
            str::stream() << "$range requires a starting value that can be represented as a 32-bit "
                             "integer, found value: "
                          << startVal.toString(),
            startVal.integral());
    uassert(34445,
            str::stream() << "$range requires a numeric ending value, found value of type: "
                          << typeName(endVal.getType()),
            endVal.numeric());
    uassert(34446,
            str::stream() << "$range requires an ending value that can be represented as a 32-bit "
                             "integer, found value: "
                          << endVal.toString(),
            endVal.integral());

    // Cast to broader type 'int64_t' to prevent overflow during loop.
    int64_t current = startVal.coerceToInt();
    int64_t end = endVal.coerceToInt();

    int64_t step = 1;
    if (vpOperand.size() == 3) {
        // A step was specified by the user.
        Value stepVal(vpOperand[2]->evaluate(root, variables));

        uassert(34447,
                str::stream() << "$range requires a numeric step value, found value of type:"
                              << typeName(stepVal.getType()),
                stepVal.numeric());
        uassert(34448,
                str::stream() << "$range requires a step value that can be represented as a 32-bit "
                                 "integer, found value: "
                              << stepVal.toString(),
                stepVal.integral());
        step = stepVal.coerceToInt();

        uassert(34449, "$range requires a non-zero step value", step != 0);
    }

    // Calculate how much memory is needed to generate the array and avoid going over the memLimit.
    auto steps = (end - current) / step;
    // If steps not positive then no amount of steps can get you from start to end. For example
    // with start=5, end=7, step=-1 steps would be negative and in this case we would return an
    // empty array.
    auto length = steps >= 0 ? 1 + steps : 0;
    int64_t memNeeded = sizeof(std::vector<Value>) + length * startVal.getApproximateSize();
    auto memLimit = internalQueryMaxRangeBytes.load();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << "$range would use too much memory (" << memNeeded << " bytes) "
                          << "and cannot spill to disk. Memory limit: "
                          << memLimit
                          << " bytes",
            memNeeded < memLimit);

    std::vector<Value> output;

    while ((step > 0 ? current < end : current > end)) {
        output.emplace_back(static_cast<int>(current));
        current += step;
    }

    return Value(std::move(output));
}

REGISTER_EXPRESSION(range, ExpressionRange::parse);
const char* ExpressionRange::getOpName() const {
    return "$range";
}

/* ------------------------ ExpressionReduce ------------------------------ */

REGISTER_EXPRESSION(reduce, ExpressionReduce::parse);
intrusive_ptr<Expression> ExpressionReduce::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    uassert(40075,
            str::stream() << "$reduce requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<ExpressionReduce> reduce(new ExpressionReduce(expCtx));

    // vpsSub is used only to parse 'in', which must have access to $$this and $$value.
    VariablesParseState vpsSub(vps);
    reduce->_thisVar = vpsSub.defineVariable("this");
    reduce->_valueVar = vpsSub.defineVariable("value");

    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "input") {
            reduce->_input = parseOperand(expCtx, elem, vps);
        } else if (field == "initialValue") {
            reduce->_initial = parseOperand(expCtx, elem, vps);
        } else if (field == "in") {
            reduce->_in = parseOperand(expCtx, elem, vpsSub);
        } else {
            uasserted(40076, str::stream() << "$reduce found an unknown argument: " << field);
        }
    }

    uassert(40077, "$reduce requires 'input' to be specified", reduce->_input);
    uassert(40078, "$reduce requires 'initialValue' to be specified", reduce->_initial);
    uassert(40079, "$reduce requires 'in' to be specified", reduce->_in);

    return reduce;
}

Value ExpressionReduce::evaluate(const Document& root, Variables* variables) const {
    Value inputVal = _input->evaluate(root, variables);

    if (inputVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40080,
            str::stream() << "$reduce requires that 'input' be an array, found: "
                          << inputVal.toString(),
            inputVal.isArray());

    Value accumulatedValue = _initial->evaluate(root, variables);

    for (auto&& elem : inputVal.getArray()) {
        variables->setValue(_thisVar, elem);
        variables->setValue(_valueVar, accumulatedValue);

        accumulatedValue = _in->evaluate(root, variables);
    }

    return accumulatedValue;
}

intrusive_ptr<Expression> ExpressionReduce::optimize() {
    _input = _input->optimize();
    _initial = _initial->optimize();
    _in = _in->optimize();
    return this;
}

void ExpressionReduce::_doAddDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    _initial->addDependencies(deps);
    _in->addDependencies(deps);
}

Value ExpressionReduce::serialize(bool explain) const {
    return Value(Document{{"$reduce",
                           Document{{"input", _input->serialize(explain)},
                                    {"initialValue", _initial->serialize(explain)},
                                    {"in", _in->serialize(explain)}}}});
}

/* ------------------------ ExpressionReverseArray ------------------------ */

Value ExpressionReverseArray::evaluate(const Document& root, Variables* variables) const {
    Value input(vpOperand[0]->evaluate(root, variables));

    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(34435,
            str::stream() << "The argument to $reverseArray must be an array, but was of type: "
                          << typeName(input.getType()),
            input.isArray());

    if (input.getArrayLength() < 2) {
        return input;
    }

    std::vector<Value> array = input.getArray();
    std::reverse(array.begin(), array.end());
    return Value(array);
}

REGISTER_EXPRESSION(reverseArray, ExpressionReverseArray::parse);
const char* ExpressionReverseArray::getOpName() const {
    return "$reverseArray";
}

namespace {
ValueSet arrayToSet(const Value& val, const ValueComparator& valueComparator) {
    const vector<Value>& array = val.getArray();
    ValueSet valueSet = valueComparator.makeOrderedValueSet();
    valueSet.insert(array.begin(), array.end());
    return valueSet;
}
}

/* ----------------------- ExpressionSetDifference ---------------------------- */

Value ExpressionSetDifference::evaluate(const Document& root, Variables* variables) const {
    const Value lhs = vpOperand[0]->evaluate(root, variables);
    const Value rhs = vpOperand[1]->evaluate(root, variables);

    if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    }

    uassert(17048,
            str::stream() << "both operands of $setDifference must be arrays. First "
                          << "argument is of type: "
                          << typeName(lhs.getType()),
            lhs.isArray());
    uassert(17049,
            str::stream() << "both operands of $setDifference must be arrays. Second "
                          << "argument is of type: "
                          << typeName(rhs.getType()),
            rhs.isArray());

    ValueSet rhsSet = arrayToSet(rhs, getExpressionContext()->getValueComparator());
    const vector<Value>& lhsArray = lhs.getArray();
    vector<Value> returnVec;

    for (vector<Value>::const_iterator it = lhsArray.begin(); it != lhsArray.end(); ++it) {
        // rhsSet serves the dual role of filtering out elements that were originally present
        // in RHS and of eleminating duplicates from LHS
        if (rhsSet.insert(*it).second) {
            returnVec.push_back(*it);
        }
    }
    return Value(std::move(returnVec));
}

REGISTER_EXPRESSION(setDifference, ExpressionSetDifference::parse);
const char* ExpressionSetDifference::getOpName() const {
    return "$setDifference";
}

/* ----------------------- ExpressionSetEquals ---------------------------- */

void ExpressionSetEquals::validateArguments(const ExpressionVector& args) const {
    uassert(17045,
            str::stream() << "$setEquals needs at least two arguments had: " << args.size(),
            args.size() >= 2);
}

Value ExpressionSetEquals::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();
    const auto& valueComparator = getExpressionContext()->getValueComparator();
    ValueSet lhs = valueComparator.makeOrderedValueSet();

    for (size_t i = 0; i < n; i++) {
        const Value nextEntry = vpOperand[i]->evaluate(root, variables);
        uassert(17044,
                str::stream() << "All operands of $setEquals must be arrays. One "
                              << "argument is of type: "
                              << typeName(nextEntry.getType()),
                nextEntry.isArray());

        if (i == 0) {
            lhs.insert(nextEntry.getArray().begin(), nextEntry.getArray().end());
        } else {
            ValueSet rhs = valueComparator.makeOrderedValueSet();
            rhs.insert(nextEntry.getArray().begin(), nextEntry.getArray().end());
            if (lhs.size() != rhs.size()) {
                return Value(false);
            }

            if (!std::equal(lhs.begin(), lhs.end(), rhs.begin(), valueComparator.getEqualTo())) {
                return Value(false);
            }
        }
    }
    return Value(true);
}

REGISTER_EXPRESSION(setEquals, ExpressionSetEquals::parse);
const char* ExpressionSetEquals::getOpName() const {
    return "$setEquals";
}

/* ----------------------- ExpressionSetIntersection ---------------------------- */

Value ExpressionSetIntersection::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();
    const auto& valueComparator = getExpressionContext()->getValueComparator();
    ValueSet currentIntersection = valueComparator.makeOrderedValueSet();
    for (size_t i = 0; i < n; i++) {
        const Value nextEntry = vpOperand[i]->evaluate(root, variables);
        if (nextEntry.nullish()) {
            return Value(BSONNULL);
        }
        uassert(17047,
                str::stream() << "All operands of $setIntersection must be arrays. One "
                              << "argument is of type: "
                              << typeName(nextEntry.getType()),
                nextEntry.isArray());

        if (i == 0) {
            currentIntersection.insert(nextEntry.getArray().begin(), nextEntry.getArray().end());
        } else {
            ValueSet nextSet = arrayToSet(nextEntry, valueComparator);
            if (currentIntersection.size() > nextSet.size()) {
                // to iterate over whichever is the smaller set
                nextSet.swap(currentIntersection);
            }
            ValueSet::iterator it = currentIntersection.begin();
            while (it != currentIntersection.end()) {
                if (!nextSet.count(*it)) {
                    ValueSet::iterator del = it;
                    ++it;
                    currentIntersection.erase(del);
                } else {
                    ++it;
                }
            }
        }
        if (currentIntersection.empty()) {
            break;
        }
    }
    return Value(vector<Value>(currentIntersection.begin(), currentIntersection.end()));
}

REGISTER_EXPRESSION(setIntersection, ExpressionSetIntersection::parse);
const char* ExpressionSetIntersection::getOpName() const {
    return "$setIntersection";
}

/* ----------------------- ExpressionSetIsSubset ---------------------------- */

namespace {
Value setIsSubsetHelper(const vector<Value>& lhs, const ValueSet& rhs) {
    // do not shortcircuit when lhs.size() > rhs.size()
    // because lhs can have redundant entries
    for (vector<Value>::const_iterator it = lhs.begin(); it != lhs.end(); ++it) {
        if (!rhs.count(*it)) {
            return Value(false);
        }
    }
    return Value(true);
}
}

Value ExpressionSetIsSubset::evaluate(const Document& root, Variables* variables) const {
    const Value lhs = vpOperand[0]->evaluate(root, variables);
    const Value rhs = vpOperand[1]->evaluate(root, variables);

    uassert(17046,
            str::stream() << "both operands of $setIsSubset must be arrays. First "
                          << "argument is of type: "
                          << typeName(lhs.getType()),
            lhs.isArray());
    uassert(17042,
            str::stream() << "both operands of $setIsSubset must be arrays. Second "
                          << "argument is of type: "
                          << typeName(rhs.getType()),
            rhs.isArray());

    return setIsSubsetHelper(lhs.getArray(),
                             arrayToSet(rhs, getExpressionContext()->getValueComparator()));
}

/**
 * This class handles the case where the RHS set is constant.
 *
 * Since it is constant we can construct the hashset once which makes the runtime performance
 * effectively constant with respect to the size of RHS. Large, constant RHS is expected to be a
 * major use case for $redact and this has been verified to improve performance significantly.
 */
class ExpressionSetIsSubset::Optimized : public ExpressionSetIsSubset {
public:
    Optimized(const boost::intrusive_ptr<ExpressionContext>& expCtx,
              const ValueSet& cachedRhsSet,
              const ExpressionVector& operands)
        : ExpressionSetIsSubset(expCtx), _cachedRhsSet(cachedRhsSet) {
        vpOperand = operands;
    }

    virtual Value evaluate(const Document& root, Variables* variables) const {
        const Value lhs = vpOperand[0]->evaluate(root, variables);

        uassert(17310,
                str::stream() << "both operands of $setIsSubset must be arrays. First "
                              << "argument is of type: "
                              << typeName(lhs.getType()),
                lhs.isArray());

        return setIsSubsetHelper(lhs.getArray(), _cachedRhsSet);
    }

private:
    const ValueSet _cachedRhsSet;
};

intrusive_ptr<Expression> ExpressionSetIsSubset::optimize() {
    // perfore basic optimizations
    intrusive_ptr<Expression> optimized = ExpressionNary::optimize();

    // if ExpressionNary::optimize() created a new value, return it directly
    if (optimized.get() != this)
        return optimized;

    if (ExpressionConstant* ec = dynamic_cast<ExpressionConstant*>(vpOperand[1].get())) {
        const Value rhs = ec->getValue();
        uassert(17311,
                str::stream() << "both operands of $setIsSubset must be arrays. Second "
                              << "argument is of type: "
                              << typeName(rhs.getType()),
                rhs.isArray());

        intrusive_ptr<Expression> optimizedWithConstant(
            new Optimized(this->getExpressionContext(),
                          arrayToSet(rhs, getExpressionContext()->getValueComparator()),
                          vpOperand));
        return optimizedWithConstant;
    }
    return optimized;
}

REGISTER_EXPRESSION(setIsSubset, ExpressionSetIsSubset::parse);
const char* ExpressionSetIsSubset::getOpName() const {
    return "$setIsSubset";
}

/* ----------------------- ExpressionSetUnion ---------------------------- */

Value ExpressionSetUnion::evaluate(const Document& root, Variables* variables) const {
    ValueSet unionedSet = getExpressionContext()->getValueComparator().makeOrderedValueSet();
    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; i++) {
        const Value newEntries = vpOperand[i]->evaluate(root, variables);
        if (newEntries.nullish()) {
            return Value(BSONNULL);
        }
        uassert(17043,
                str::stream() << "All operands of $setUnion must be arrays. One argument"
                              << " is of type: "
                              << typeName(newEntries.getType()),
                newEntries.isArray());

        unionedSet.insert(newEntries.getArray().begin(), newEntries.getArray().end());
    }
    return Value(vector<Value>(unionedSet.begin(), unionedSet.end()));
}

REGISTER_EXPRESSION(setUnion, ExpressionSetUnion::parse);
const char* ExpressionSetUnion::getOpName() const {
    return "$setUnion";
}

/* ----------------------- ExpressionIsArray ---------------------------- */

Value ExpressionIsArray::evaluate(const Document& root, Variables* variables) const {
    Value argument = vpOperand[0]->evaluate(root, variables);
    return Value(argument.isArray());
}

REGISTER_EXPRESSION(isArray, ExpressionIsArray::parse);
const char* ExpressionIsArray::getOpName() const {
    return "$isArray";
}

/* ----------------------- ExpressionSlice ---------------------------- */

Value ExpressionSlice::evaluate(const Document& root, Variables* variables) const {
    const size_t n = vpOperand.size();

    Value arrayVal = vpOperand[0]->evaluate(root, variables);
    // Could be either a start index or the length from 0.
    Value arg2 = vpOperand[1]->evaluate(root, variables);

    if (arrayVal.nullish() || arg2.nullish()) {
        return Value(BSONNULL);
    }

    uassert(28724,
            str::stream() << "First argument to $slice must be an array, but is"
                          << " of type: "
                          << typeName(arrayVal.getType()),
            arrayVal.isArray());
    uassert(28725,
            str::stream() << "Second argument to $slice must be a numeric value,"
                          << " but is of type: "
                          << typeName(arg2.getType()),
            arg2.numeric());
    uassert(28726,
            str::stream() << "Second argument to $slice can't be represented as"
                          << " a 32-bit integer: "
                          << arg2.coerceToDouble(),
            arg2.integral());

    const auto& array = arrayVal.getArray();
    size_t start;
    size_t end;

    if (n == 2) {
        // Only count given.
        int count = arg2.coerceToInt();
        start = 0;
        end = array.size();
        if (count >= 0) {
            end = std::min(end, size_t(count));
        } else {
            // Negative count's start from the back. If a abs(count) is greater
            // than the
            // length of the array, return the whole array.
            start = std::max(0, static_cast<int>(array.size()) + count);
        }
    } else {
        // We have both a start index and a count.
        int startInt = arg2.coerceToInt();
        if (startInt < 0) {
            // Negative values start from the back. If a abs(start) is greater
            // than the length
            // of the array, start from 0.
            start = std::max(0, static_cast<int>(array.size()) + startInt);
        } else {
            start = std::min(array.size(), size_t(startInt));
        }

        Value countVal = vpOperand[2]->evaluate(root, variables);

        if (countVal.nullish()) {
            return Value(BSONNULL);
        }

        uassert(28727,
                str::stream() << "Third argument to $slice must be numeric, but "
                              << "is of type: "
                              << typeName(countVal.getType()),
                countVal.numeric());
        uassert(28728,
                str::stream() << "Third argument to $slice can't be represented"
                              << " as a 32-bit integer: "
                              << countVal.coerceToDouble(),
                countVal.integral());
        uassert(28729,
                str::stream() << "Third argument to $slice must be positive: "
                              << countVal.coerceToInt(),
                countVal.coerceToInt() > 0);

        size_t count = size_t(countVal.coerceToInt());
        end = std::min(start + count, array.size());
    }

    return Value(vector<Value>(array.begin() + start, array.begin() + end));
}

REGISTER_EXPRESSION(slice, ExpressionSlice::parse);
const char* ExpressionSlice::getOpName() const {
    return "$slice";
}

/* ----------------------- ExpressionSize ---------------------------- */

Value ExpressionSize::evaluate(const Document& root, Variables* variables) const {
    Value array = vpOperand[0]->evaluate(root, variables);

    uassert(17124,
            str::stream() << "The argument to $size must be an array, but was of type: "
                          << typeName(array.getType()),
            array.isArray());
    return Value::createIntOrLong(array.getArray().size());
}

REGISTER_EXPRESSION(size, ExpressionSize::parse);
const char* ExpressionSize::getOpName() const {
    return "$size";
}

/* ----------------------- ExpressionSplit --------------------------- */

Value ExpressionSplit::evaluate(const Document& root, Variables* variables) const {
    Value inputArg = vpOperand[0]->evaluate(root, variables);
    Value separatorArg = vpOperand[1]->evaluate(root, variables);

    if (inputArg.nullish() || separatorArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40085,
            str::stream() << "$split requires an expression that evaluates to a string as a first "
                             "argument, found: "
                          << typeName(inputArg.getType()),
            inputArg.getType() == BSONType::String);
    uassert(40086,
            str::stream() << "$split requires an expression that evaluates to a string as a second "
                             "argument, found: "
                          << typeName(separatorArg.getType()),
            separatorArg.getType() == BSONType::String);

    std::string input = inputArg.getString();
    std::string separator = separatorArg.getString();

    uassert(40087, "$split requires a non-empty separator", !separator.empty());

    std::vector<Value> output;

    // Keep track of the index at which the current output string began.
    size_t splitStartIndex = 0;

    // Iterate through 'input' and check to see if 'separator' matches at any point.
    for (size_t i = 0; i < input.size();) {
        if (stringHasTokenAtIndex(i, input, separator)) {
            // We matched; add the current string to our output and jump ahead.
            StringData splitString(input.c_str() + splitStartIndex, i - splitStartIndex);
            output.push_back(Value(splitString));
            i += separator.size();
            splitStartIndex = i;
        } else {
            // We did not match, continue to the next character.
            ++i;
        }
    }

    StringData splitString(input.c_str() + splitStartIndex, input.size() - splitStartIndex);
    output.push_back(Value(splitString));

    return Value(output);
}

REGISTER_EXPRESSION(split, ExpressionSplit::parse);
const char* ExpressionSplit::getOpName() const {
    return "$split";
}

/* ----------------------- ExpressionSqrt ---------------------------- */

Value ExpressionSqrt::evaluateNumericArg(const Value& numericArg) const {
    auto checkArg = [](bool nonNegative) {
        uassert(28714, "$sqrt's argument must be greater than or equal to 0", nonNegative);
    };

    if (numericArg.getType() == NumberDecimal) {
        Decimal128 argDec = numericArg.getDecimal();
        checkArg(!argDec.isLess(Decimal128::kNormalizedZero));  // NaN returns Nan without error
        return Value(argDec.squareRoot());
    }
    double argDouble = numericArg.coerceToDouble();
    checkArg(!(argDouble < 0));  // NaN returns Nan without error
    return Value(sqrt(argDouble));
}

REGISTER_EXPRESSION(sqrt, ExpressionSqrt::parse);
const char* ExpressionSqrt::getOpName() const {
    return "$sqrt";
}

/* ----------------------- ExpressionStrcasecmp ---------------------------- */

Value ExpressionStrcasecmp::evaluate(const Document& root, Variables* variables) const {
    Value pString1(vpOperand[0]->evaluate(root, variables));
    Value pString2(vpOperand[1]->evaluate(root, variables));

    /* boost::iequals returns a bool not an int so strings must actually be allocated */
    string str1 = boost::to_upper_copy(pString1.coerceToString());
    string str2 = boost::to_upper_copy(pString2.coerceToString());
    int result = str1.compare(str2);

    if (result == 0)
        return Value(0);
    else if (result > 0)
        return Value(1);
    else
        return Value(-1);
}

REGISTER_EXPRESSION(strcasecmp, ExpressionStrcasecmp::parse);
const char* ExpressionStrcasecmp::getOpName() const {
    return "$strcasecmp";
}

/* ----------------------- ExpressionSubstrBytes ---------------------------- */

Value ExpressionSubstrBytes::evaluate(const Document& root, Variables* variables) const {
    Value pString(vpOperand[0]->evaluate(root, variables));
    Value pLower(vpOperand[1]->evaluate(root, variables));
    Value pLength(vpOperand[2]->evaluate(root, variables));

    string str = pString.coerceToString();
    uassert(16034,
            str::stream() << getOpName()
                          << ":  starting index must be a numeric type (is BSON type "
                          << typeName(pLower.getType())
                          << ")",
            (pLower.getType() == NumberInt || pLower.getType() == NumberLong ||
             pLower.getType() == NumberDouble));
    uassert(16035,
            str::stream() << getOpName() << ":  length must be a numeric type (is BSON type "
                          << typeName(pLength.getType())
                          << ")",
            (pLength.getType() == NumberInt || pLength.getType() == NumberLong ||
             pLength.getType() == NumberDouble));

    const long long signedLower = pLower.coerceToLong();

    uassert(50752,
            str::stream() << getOpName() << ":  starting index must be non-negative (got: "
                          << signedLower
                          << ")",
            signedLower >= 0);

    const string::size_type lower = static_cast<string::size_type>(signedLower);

    // If the passed length is negative, we should return the rest of the string.
    const long long signedLength = pLength.coerceToLong();
    const string::size_type length =
        signedLength < 0 ? str.length() : static_cast<string::size_type>(signedLength);

    uassert(28656,
            str::stream() << getOpName()
                          << ":  Invalid range, starting index is a UTF-8 continuation byte.",
            (lower >= str.length() || !str::isUTF8ContinuationByte(str[lower])));

    // Check the byte after the last character we'd return. If it is a continuation byte, that
    // means we're in the middle of a UTF-8 character.
    uassert(
        28657,
        str::stream() << getOpName()
                      << ":  Invalid range, ending index is in the middle of a UTF-8 character.",
        (lower + length >= str.length() || !str::isUTF8ContinuationByte(str[lower + length])));

    if (lower >= str.length()) {
        // If lower > str.length() then string::substr() will throw out_of_range, so return an
        // empty string if lower is not a valid string index.
        return Value(StringData());
    }
    return Value(str.substr(lower, length));
}

// $substr is deprecated in favor of $substrBytes, but for now will just parse into a $substrBytes.
REGISTER_EXPRESSION(substrBytes, ExpressionSubstrBytes::parse);
REGISTER_EXPRESSION(substr, ExpressionSubstrBytes::parse);
const char* ExpressionSubstrBytes::getOpName() const {
    return "$substrBytes";
}

/* ----------------------- ExpressionSubstrCP ---------------------------- */

Value ExpressionSubstrCP::evaluate(const Document& root, Variables* variables) const {
    Value inputVal(vpOperand[0]->evaluate(root, variables));
    Value lowerVal(vpOperand[1]->evaluate(root, variables));
    Value lengthVal(vpOperand[2]->evaluate(root, variables));

    std::string str = inputVal.coerceToString();
    uassert(34450,
            str::stream() << getOpName() << ": starting index must be a numeric type (is BSON type "
                          << typeName(lowerVal.getType())
                          << ")",
            lowerVal.numeric());
    uassert(34451,
            str::stream() << getOpName()
                          << ": starting index cannot be represented as a 32-bit integral value: "
                          << lowerVal.toString(),
            lowerVal.integral());
    uassert(34452,
            str::stream() << getOpName() << ": length must be a numeric type (is BSON type "
                          << typeName(lengthVal.getType())
                          << ")",
            lengthVal.numeric());
    uassert(34453,
            str::stream() << getOpName()
                          << ": length cannot be represented as a 32-bit integral value: "
                          << lengthVal.toString(),
            lengthVal.integral());

    int startIndexCodePoints = lowerVal.coerceToInt();
    int length = lengthVal.coerceToInt();

    uassert(34454,
            str::stream() << getOpName() << ": length must be a nonnegative integer.",
            length >= 0);

    uassert(34455,
            str::stream() << getOpName() << ": the starting index must be nonnegative integer.",
            startIndexCodePoints >= 0);

    size_t startIndexBytes = 0;

    for (int i = 0; i < startIndexCodePoints; i++) {
        if (startIndexBytes >= str.size()) {
            return Value(StringData());
        }
        uassert(34456,
                str::stream() << getOpName() << ": invalid UTF-8 string",
                !str::isUTF8ContinuationByte(str[startIndexBytes]));
        size_t codePointLength = getCodePointLength(str[startIndexBytes]);
        uassert(
            34457, str::stream() << getOpName() << ": invalid UTF-8 string", codePointLength <= 4);
        startIndexBytes += codePointLength;
    }

    size_t endIndexBytes = startIndexBytes;

    for (int i = 0; i < length && endIndexBytes < str.size(); i++) {
        uassert(34458,
                str::stream() << getOpName() << ": invalid UTF-8 string",
                !str::isUTF8ContinuationByte(str[endIndexBytes]));
        size_t codePointLength = getCodePointLength(str[endIndexBytes]);
        uassert(
            34459, str::stream() << getOpName() << ": invalid UTF-8 string", codePointLength <= 4);
        endIndexBytes += codePointLength;
    }

    return Value(std::string(str, startIndexBytes, endIndexBytes - startIndexBytes));
}

REGISTER_EXPRESSION(substrCP, ExpressionSubstrCP::parse);
const char* ExpressionSubstrCP::getOpName() const {
    return "$substrCP";
}

/* ----------------------- ExpressionStrLenBytes ------------------------- */

Value ExpressionStrLenBytes::evaluate(const Document& root, Variables* variables) const {
    Value str(vpOperand[0]->evaluate(root, variables));

    uassert(34473,
            str::stream() << "$strLenBytes requires a string argument, found: "
                          << typeName(str.getType()),
            str.getType() == String);

    size_t strLen = str.getString().size();

    uassert(34470,
            "string length could not be represented as an int.",
            strLen <= std::numeric_limits<int>::max());
    return Value(static_cast<int>(strLen));
}

REGISTER_EXPRESSION(strLenBytes, ExpressionStrLenBytes::parse);
const char* ExpressionStrLenBytes::getOpName() const {
    return "$strLenBytes";
}

/* ----------------------- ExpressionStrLenCP ------------------------- */

Value ExpressionStrLenCP::evaluate(const Document& root, Variables* variables) const {
    Value val(vpOperand[0]->evaluate(root, variables));

    uassert(34471,
            str::stream() << "$strLenCP requires a string argument, found: "
                          << typeName(val.getType()),
            val.getType() == String);

    std::string stringVal = val.getString();
    size_t strLen = str::lengthInUTF8CodePoints(stringVal);

    uassert(34472,
            "string length could not be represented as an int.",
            strLen <= std::numeric_limits<int>::max());

    return Value(static_cast<int>(strLen));
}

REGISTER_EXPRESSION(strLenCP, ExpressionStrLenCP::parse);
const char* ExpressionStrLenCP::getOpName() const {
    return "$strLenCP";
}

/* ----------------------- ExpressionSubtract ---------------------------- */

Value ExpressionSubtract::evaluate(const Document& root, Variables* variables) const {
    Value lhs = vpOperand[0]->evaluate(root, variables);
    Value rhs = vpOperand[1]->evaluate(root, variables);

    BSONType diffType = Value::getWidestNumeric(rhs.getType(), lhs.getType());

    if (diffType == NumberDecimal) {
        Decimal128 right = rhs.coerceToDecimal();
        Decimal128 left = lhs.coerceToDecimal();
        return Value(left.subtract(right));
    } else if (diffType == NumberDouble) {
        double right = rhs.coerceToDouble();
        double left = lhs.coerceToDouble();
        return Value(left - right);
    } else if (diffType == NumberLong) {
        long long result;

        // If there is an overflow, convert the values to doubles.
        if (mongoSignedSubtractOverflow64(lhs.coerceToLong(), rhs.coerceToLong(), &result)) {
            return Value(lhs.coerceToDouble() - rhs.coerceToDouble());
        }
        return Value(result);
    } else if (diffType == NumberInt) {
        long long right = rhs.coerceToLong();
        long long left = lhs.coerceToLong();
        return Value::createIntOrLong(left - right);
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else if (lhs.getType() == Date) {
        if (rhs.getType() == Date) {
            return Value(durationCount<Milliseconds>(lhs.getDate() - rhs.getDate()));
        } else if (rhs.numeric()) {
            return Value(lhs.getDate() - Milliseconds(rhs.coerceToLong()));
        } else {
            uasserted(16613,
                      str::stream() << "cant $subtract a " << typeName(rhs.getType())
                                    << " from a Date");
        }
    } else {
        uasserted(16556,
                  str::stream() << "cant $subtract a" << typeName(rhs.getType()) << " from a "
                                << typeName(lhs.getType()));
    }
}

REGISTER_EXPRESSION(subtract, ExpressionSubtract::parse);
const char* ExpressionSubtract::getOpName() const {
    return "$subtract";
}

/* ------------------------- ExpressionSwitch ------------------------------ */

REGISTER_EXPRESSION(switch, ExpressionSwitch::parse);

Value ExpressionSwitch::evaluate(const Document& root, Variables* variables) const {
    for (auto&& branch : _branches) {
        Value caseExpression(branch.first->evaluate(root, variables));

        if (caseExpression.coerceToBool()) {
            return branch.second->evaluate(root, variables);
        }
    }

    uassert(40066,
            "$switch could not find a matching branch for an input, and no default was specified.",
            _default);

    return _default->evaluate(root, variables);
}

boost::intrusive_ptr<Expression> ExpressionSwitch::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    uassert(40060,
            str::stream() << "$switch requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<ExpressionSwitch> expression(new ExpressionSwitch(expCtx));

    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "branches") {
            // Parse each branch separately.
            uassert(40061,
                    str::stream() << "$switch expected an array for 'branches', found: "
                                  << typeName(elem.type()),
                    elem.type() == Array);

            for (auto&& branch : elem.Array()) {
                uassert(40062,
                        str::stream() << "$switch expected each branch to be an object, found: "
                                      << typeName(branch.type()),
                        branch.type() == Object);

                ExpressionPair branchExpression;

                for (auto&& branchElement : branch.Obj()) {
                    auto branchField = branchElement.fieldNameStringData();

                    if (branchField == "case") {
                        branchExpression.first = parseOperand(expCtx, branchElement, vps);
                    } else if (branchField == "then") {
                        branchExpression.second = parseOperand(expCtx, branchElement, vps);
                    } else {
                        uasserted(40063,
                                  str::stream() << "$switch found an unknown argument to a branch: "
                                                << branchField);
                    }
                }

                uassert(40064,
                        "$switch requires each branch have a 'case' expression",
                        branchExpression.first);
                uassert(40065,
                        "$switch requires each branch have a 'then' expression.",
                        branchExpression.second);

                expression->_branches.push_back(branchExpression);
            }
        } else if (field == "default") {
            // Optional, arbitrary expression.
            expression->_default = parseOperand(expCtx, elem, vps);
        } else {
            uasserted(40067, str::stream() << "$switch found an unknown argument: " << field);
        }
    }

    uassert(40068, "$switch requires at least one branch.", !expression->_branches.empty());

    return expression;
}

void ExpressionSwitch::_doAddDependencies(DepsTracker* deps) const {
    for (auto&& branch : _branches) {
        branch.first->addDependencies(deps);
        branch.second->addDependencies(deps);
    }

    if (_default) {
        _default->addDependencies(deps);
    }
}

boost::intrusive_ptr<Expression> ExpressionSwitch::optimize() {
    if (_default) {
        _default = _default->optimize();
    }

    std::transform(_branches.begin(),
                   _branches.end(),
                   _branches.begin(),
                   [](ExpressionPair branch) -> ExpressionPair {
                       return {branch.first->optimize(), branch.second->optimize()};
                   });

    return this;
}

Value ExpressionSwitch::serialize(bool explain) const {
    std::vector<Value> serializedBranches;
    serializedBranches.reserve(_branches.size());

    for (auto&& branch : _branches) {
        serializedBranches.push_back(Value(Document{{"case", branch.first->serialize(explain)},
                                                    {"then", branch.second->serialize(explain)}}));
    }

    if (_default) {
        return Value(Document{{"$switch",
                               Document{{"branches", Value(serializedBranches)},
                                        {"default", _default->serialize(explain)}}}});
    }

    return Value(Document{{"$switch", Document{{"branches", Value(serializedBranches)}}}});
}

/* ------------------------- ExpressionToLower ----------------------------- */

Value ExpressionToLower::evaluate(const Document& root, Variables* variables) const {
    Value pString(vpOperand[0]->evaluate(root, variables));
    string str = pString.coerceToString();
    boost::to_lower(str);
    return Value(str);
}

REGISTER_EXPRESSION(toLower, ExpressionToLower::parse);
const char* ExpressionToLower::getOpName() const {
    return "$toLower";
}

/* ------------------------- ExpressionToUpper -------------------------- */

Value ExpressionToUpper::evaluate(const Document& root, Variables* variables) const {
    Value pString(vpOperand[0]->evaluate(root, variables));
    string str(pString.coerceToString());
    boost::to_upper(str);
    return Value(str);
}

REGISTER_EXPRESSION(toUpper, ExpressionToUpper::parse);
const char* ExpressionToUpper::getOpName() const {
    return "$toUpper";
}

/* -------------------------- ExpressionTrim ------------------------------ */

REGISTER_EXPRESSION_WITH_MIN_VERSION(
    trim,
    ExpressionTrim::parse,
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

REGISTER_EXPRESSION_WITH_MIN_VERSION(
    ltrim,
    ExpressionTrim::parse,
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

REGISTER_EXPRESSION_WITH_MIN_VERSION(
    rtrim,
    ExpressionTrim::parse,
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

intrusive_ptr<Expression> ExpressionTrim::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    const auto name = expr.fieldNameStringData();
    TrimType trimType = TrimType::kBoth;
    if (name == "$ltrim"_sd) {
        trimType = TrimType::kLeft;
    } else if (name == "$rtrim"_sd) {
        trimType = TrimType::kRight;
    } else {
        invariant(name == "$trim"_sd);
    }
    uassert(50696,
            str::stream() << name << " only supports an object as an argument, found "
                          << typeName(expr.type()),
            expr.type() == Object);

    boost::intrusive_ptr<Expression> input;
    boost::intrusive_ptr<Expression> characters;
    for (auto&& elem : expr.Obj()) {
        const auto field = elem.fieldNameStringData();
        if (field == "input"_sd) {
            input = parseOperand(expCtx, elem, vps);
        } else if (field == "chars"_sd) {
            characters = parseOperand(expCtx, elem, vps);
        } else {
            uasserted(50694,
                      str::stream() << name << " found an unknown argument: " << elem.fieldName());
        }
    }
    uassert(50695, str::stream() << name << " requires an 'input' field", input);

    return new ExpressionTrim(expCtx, trimType, name, input, characters);
}

namespace {
const std::vector<StringData> kDefaultTrimWhitespaceChars = {
    "\0"_sd,      // Null character. Avoid using "\u0000" syntax to work around a gcc bug:
                  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53690.
    "\u0020"_sd,  // Space
    "\u0009"_sd,  // Horizontal tab
    "\u000A"_sd,  // Line feed/new line
    "\u000B"_sd,  // Vertical tab
    "\u000C"_sd,  // Form feed
    "\u000D"_sd,  // Horizontal tab
    "\u00A0"_sd,  // Non-breaking space
    "\u1680"_sd,  // Ogham space mark
    "\u2000"_sd,  // En quad
    "\u2001"_sd,  // Em quad
    "\u2002"_sd,  // En space
    "\u2003"_sd,  // Em space
    "\u2004"_sd,  // Three-per-em space
    "\u2005"_sd,  // Four-per-em space
    "\u2006"_sd,  // Six-per-em space
    "\u2007"_sd,  // Figure space
    "\u2008"_sd,  // Punctuation space
    "\u2009"_sd,  // Thin space
    "\u200A"_sd   // Hair space
};

/**
 * Assuming 'charByte' is the beginning of a UTF-8 code point, returns the number of bytes that
 * should be used to represent the code point. Said another way, computes how many continuation
 * bytes are expected to be present after 'charByte' in a UTF-8 encoded string.
 */
inline size_t numberOfBytesForCodePoint(char charByte) {
    if ((charByte & 0b11111000) == 0b11110000) {
        return 4;
    } else if ((charByte & 0b11110000) == 0b11100000) {
        return 3;
    } else if ((charByte & 0b11100000) == 0b11000000) {
        return 2;
    } else {
        return 1;
    }
}

/**
 * Returns a vector with one entry per code point to trim, or throws an exception if 'utf8String'
 * contains invalid UTF-8.
 */
std::vector<StringData> extractCodePointsFromChars(StringData utf8String,
                                                   StringData expressionName) {
    std::vector<StringData> codePoints;
    std::size_t i = 0;
    while (i < utf8String.size()) {
        uassert(50698,
                str::stream() << "Failed to parse \"chars\" argument to " << expressionName
                              << ": Detected invalid UTF-8. Got continuation byte when expecting "
                                 "the start of a new code point.",
                !str::isUTF8ContinuationByte(utf8String[i]));
        codePoints.push_back(utf8String.substr(i, numberOfBytesForCodePoint(utf8String[i])));
        i += numberOfBytesForCodePoint(utf8String[i]);
    }
    uassert(50697,
            str::stream()
                << "Failed to parse \"chars\" argument to "
                << expressionName
                << ": Detected invalid UTF-8. Missing expected continuation byte at end of string.",
            i <= utf8String.size());
    return codePoints;
}
}  // namespace

Value ExpressionTrim::evaluate(const Document& root, Variables* variables) const {
    auto unvalidatedInput = _input->evaluate(root, variables);
    if (unvalidatedInput.nullish()) {
        return Value(BSONNULL);
    }
    uassert(50699,
            str::stream() << _name << " requires its input to be a string, got "
                          << unvalidatedInput.toString()
                          << " (of type "
                          << typeName(unvalidatedInput.getType())
                          << ") instead.",
            unvalidatedInput.getType() == BSONType::String);
    const StringData input(unvalidatedInput.getStringData());

    if (!_characters) {
        return Value(doTrim(input, kDefaultTrimWhitespaceChars));
    }
    auto unvalidatedUserChars = _characters->evaluate(root, variables);
    if (unvalidatedUserChars.nullish()) {
        return Value(BSONNULL);
    }
    uassert(50700,
            str::stream() << _name << " requires 'chars' to be a string, got "
                          << unvalidatedUserChars.toString()
                          << " (of type "
                          << typeName(unvalidatedUserChars.getType())
                          << ") instead.",
            unvalidatedUserChars.getType() == BSONType::String);

    return Value(
        doTrim(input, extractCodePointsFromChars(unvalidatedUserChars.getStringData(), _name)));
}

bool ExpressionTrim::codePointMatchesAtIndex(const StringData& input,
                                             std::size_t indexOfInput,
                                             const StringData& testCP) {
    for (size_t i = 0; i < testCP.size(); ++i) {
        if (indexOfInput + i >= input.size() || input[indexOfInput + i] != testCP[i]) {
            return false;
        }
    }
    return true;
};

StringData ExpressionTrim::trimFromLeft(StringData input, const std::vector<StringData>& trimCPs) {
    std::size_t bytesTrimmedFromLeft = 0u;
    while (bytesTrimmedFromLeft < input.size()) {
        // Look for any matching code point to trim.
        auto matchingCP = std::find_if(trimCPs.begin(), trimCPs.end(), [&](auto& testCP) {
            return codePointMatchesAtIndex(input, bytesTrimmedFromLeft, testCP);
        });
        if (matchingCP == trimCPs.end()) {
            // Nothing to trim, stop here.
            break;
        }
        bytesTrimmedFromLeft += matchingCP->size();
    }
    return input.substr(bytesTrimmedFromLeft);
}

StringData ExpressionTrim::trimFromRight(StringData input, const std::vector<StringData>& trimCPs) {
    std::size_t bytesTrimmedFromRight = 0u;
    while (bytesTrimmedFromRight < input.size()) {
        std::size_t indexToTrimFrom = input.size() - bytesTrimmedFromRight;
        auto matchingCP = std::find_if(trimCPs.begin(), trimCPs.end(), [&](auto& testCP) {
            if (indexToTrimFrom < testCP.size()) {
                // We've gone off the left of the string.
                return false;
            }
            return codePointMatchesAtIndex(input, indexToTrimFrom - testCP.size(), testCP);
        });
        if (matchingCP == trimCPs.end()) {
            // Nothing to trim, stop here.
            break;
        }
        bytesTrimmedFromRight += matchingCP->size();
    }
    return input.substr(0, input.size() - bytesTrimmedFromRight);
}

StringData ExpressionTrim::doTrim(StringData input, const std::vector<StringData>& trimCPs) const {
    if (_trimType == TrimType::kBoth || _trimType == TrimType::kLeft) {
        input = trimFromLeft(input, trimCPs);
    }
    if (_trimType == TrimType::kBoth || _trimType == TrimType::kRight) {
        input = trimFromRight(input, trimCPs);
    }
    return input;
}

boost::intrusive_ptr<Expression> ExpressionTrim::optimize() {
    _input = _input->optimize();
    if (_characters) {
        _characters = _characters->optimize();
    }
    if (ExpressionConstant::allNullOrConstant({_input, _characters})) {
        return ExpressionConstant::create(
            getExpressionContext(),
            this->evaluate(Document(), &(getExpressionContext()->variables)));
    }
    return this;
}

Value ExpressionTrim::serialize(bool explain) const {
    return Value(
        Document{{_name,
                  Document{{"input", _input->serialize(explain)},
                           {"chars", _characters ? _characters->serialize(explain) : Value()}}}});
}

void ExpressionTrim::_doAddDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    if (_characters) {
        _characters->addDependencies(deps);
    }
}

/* ------------------------- ExpressionTrunc -------------------------- */

Value ExpressionTrunc::evaluateNumericArg(const Value& numericArg) const {
    // There's no point in truncating integers or longs, it will have no effect.

    switch (numericArg.getType()) {
        case NumberDecimal:
            return Value(numericArg.getDecimal().quantize(Decimal128::kNormalizedZero,
                                                          Decimal128::kRoundTowardZero));
        case NumberDouble:
            return Value(std::trunc(numericArg.getDouble()));
        default:
            return numericArg;
    }
}

REGISTER_EXPRESSION(trunc, ExpressionTrunc::parse);
const char* ExpressionTrunc::getOpName() const {
    return "$trunc";
}

/* ------------------------- ExpressionType ----------------------------- */

Value ExpressionType::evaluate(const Document& root, Variables* variables) const {
    Value val(vpOperand[0]->evaluate(root, variables));
    return Value(StringData(typeName(val.getType())));
}

REGISTER_EXPRESSION(type, ExpressionType::parse);
const char* ExpressionType::getOpName() const {
    return "$type";
}

/* -------------------------- ExpressionZip ------------------------------ */

REGISTER_EXPRESSION(zip, ExpressionZip::parse);
intrusive_ptr<Expression> ExpressionZip::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    uassert(34460,
            str::stream() << "$zip only supports an object as an argument, found "
                          << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<ExpressionZip> newZip(new ExpressionZip(expCtx));

    for (auto&& elem : expr.Obj()) {
        const auto field = elem.fieldNameStringData();
        if (field == "inputs") {
            uassert(34461,
                    str::stream() << "inputs must be an array of expressions, found "
                                  << typeName(elem.type()),
                    elem.type() == Array);
            for (auto&& subExpr : elem.Array()) {
                newZip->_inputs.push_back(parseOperand(expCtx, subExpr, vps));
            }
        } else if (field == "defaults") {
            uassert(34462,
                    str::stream() << "defaults must be an array of expressions, found "
                                  << typeName(elem.type()),
                    elem.type() == Array);
            for (auto&& subExpr : elem.Array()) {
                newZip->_defaults.push_back(parseOperand(expCtx, subExpr, vps));
            }
        } else if (field == "useLongestLength") {
            uassert(34463,
                    str::stream() << "useLongestLength must be a bool, found "
                                  << typeName(expr.type()),
                    elem.type() == Bool);
            newZip->_useLongestLength = elem.Bool();
        } else {
            uasserted(34464,
                      str::stream() << "$zip found an unknown argument: " << elem.fieldName());
        }
    }

    uassert(34465, "$zip requires at least one input array", !newZip->_inputs.empty());
    uassert(34466,
            "cannot specify defaults unless useLongestLength is true",
            (newZip->_useLongestLength || newZip->_defaults.empty()));
    uassert(34467,
            "defaults and inputs must have the same length",
            (newZip->_defaults.empty() || newZip->_defaults.size() == newZip->_inputs.size()));

    return std::move(newZip);
}

Value ExpressionZip::evaluate(const Document& root, Variables* variables) const {
    // Evaluate input values.
    vector<vector<Value>> inputValues;
    inputValues.reserve(_inputs.size());

    size_t minArraySize = 0;
    size_t maxArraySize = 0;
    for (size_t i = 0; i < _inputs.size(); i++) {
        Value evalExpr = _inputs[i].get()->evaluate(root, variables);
        if (evalExpr.nullish()) {
            return Value(BSONNULL);
        }

        uassert(34468,
                str::stream() << "$zip found a non-array expression in input: "
                              << evalExpr.toString(),
                evalExpr.isArray());

        inputValues.push_back(evalExpr.getArray());

        size_t arraySize = evalExpr.getArrayLength();

        if (i == 0) {
            minArraySize = arraySize;
            maxArraySize = arraySize;
        } else {
            auto arraySizes = std::minmax({minArraySize, arraySize, maxArraySize});
            minArraySize = arraySizes.first;
            maxArraySize = arraySizes.second;
        }
    }

    vector<Value> evaluatedDefaults(_inputs.size(), Value(BSONNULL));

    // If we need default values, evaluate each expression.
    if (minArraySize != maxArraySize) {
        for (size_t i = 0; i < _defaults.size(); i++) {
            evaluatedDefaults[i] = _defaults[i].get()->evaluate(root, variables);
        }
    }

    size_t outputLength = _useLongestLength ? maxArraySize : minArraySize;

    // The final output array, e.g. [[1, 2, 3], [2, 3, 4]].
    vector<Value> output;

    // Used to construct each array in the output, e.g. [1, 2, 3].
    vector<Value> outputChild;

    output.reserve(outputLength);
    outputChild.reserve(_inputs.size());

    for (size_t row = 0; row < outputLength; row++) {
        outputChild.clear();
        for (size_t col = 0; col < _inputs.size(); col++) {
            if (inputValues[col].size() > row) {
                // Add the value from the appropriate input array.
                outputChild.push_back(inputValues[col][row]);
            } else {
                // Add the corresponding default value.
                outputChild.push_back(evaluatedDefaults[col]);
            }
        }
        output.push_back(Value(outputChild));
    }

    return Value(output);
}

boost::intrusive_ptr<Expression> ExpressionZip::optimize() {
    std::transform(_inputs.begin(),
                   _inputs.end(),
                   _inputs.begin(),
                   [](intrusive_ptr<Expression> inputExpression) -> intrusive_ptr<Expression> {
                       return inputExpression->optimize();
                   });

    std::transform(_defaults.begin(),
                   _defaults.end(),
                   _defaults.begin(),
                   [](intrusive_ptr<Expression> defaultExpression) -> intrusive_ptr<Expression> {
                       return defaultExpression->optimize();
                   });

    return this;
}

Value ExpressionZip::serialize(bool explain) const {
    vector<Value> serializedInput;
    vector<Value> serializedDefaults;
    Value serializedUseLongestLength = Value(_useLongestLength);

    for (auto&& expr : _inputs) {
        serializedInput.push_back(expr->serialize(explain));
    }

    for (auto&& expr : _defaults) {
        serializedDefaults.push_back(expr->serialize(explain));
    }

    return Value(DOC("$zip" << DOC("inputs" << Value(serializedInput) << "defaults"
                                            << Value(serializedDefaults)
                                            << "useLongestLength"
                                            << serializedUseLongestLength)));
}

void ExpressionZip::_doAddDependencies(DepsTracker* deps) const {
    std::for_each(
        _inputs.begin(), _inputs.end(), [&deps](intrusive_ptr<Expression> inputExpression) -> void {
            inputExpression->addDependencies(deps);
        });
    std::for_each(_defaults.begin(),
                  _defaults.end(),
                  [&deps](intrusive_ptr<Expression> defaultExpression) -> void {
                      defaultExpression->addDependencies(deps);
                  });
}

/* -------------------------- ExpressionConvert ------------------------------ */

namespace {

/**
 * $convert supports a big grab bag of conversions, so ConversionTable maintains a collection of
 * conversion functions, as well as a table to organize them by inputType and targetType.
 */
class ConversionTable {
public:
    using ConversionFunc =
        std::function<Value(const boost::intrusive_ptr<ExpressionContext>&, Value)>;

    ConversionTable() {
        //
        // Conversions from NumberDouble
        //
        table[BSONType::NumberDouble][BSONType::NumberDouble] = &performIdentityConversion;
        table[BSONType::NumberDouble][BSONType::String] = &performFormatDouble;
        table[BSONType::NumberDouble]
             [BSONType::Bool] = [](const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   Value inputValue) { return Value(inputValue.coerceToBool()); };
        table[BSONType::NumberDouble][BSONType::Date] = &performCastNumberToDate;
        table[BSONType::NumberDouble][BSONType::NumberInt] = &performCastDoubleToInt;
        table[BSONType::NumberDouble][BSONType::NumberLong] = &performCastDoubleToLong;
        table[BSONType::NumberDouble][BSONType::NumberDecimal] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.coerceToDecimal());
            };

        //
        // Conversions from String
        //
        table[BSONType::String][BSONType::NumberDouble] = &parseStringToNumber<double, 0>;
        table[BSONType::String][BSONType::String] = &performIdentityConversion;
        table[BSONType::String][BSONType::jstOID] = &parseStringToOID;
        table[BSONType::String][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::String][BSONType::Date] = [](
            const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
            return Value(expCtx->timeZoneDatabase->fromString(inputValue.getStringData(),
                                                              mongo::TimeZoneDatabase::utcZone()));
        };
        table[BSONType::String][BSONType::NumberInt] = &parseStringToNumber<int, 10>;
        table[BSONType::String][BSONType::NumberLong] = &parseStringToNumber<long long, 10>;
        table[BSONType::String][BSONType::NumberDecimal] = &parseStringToNumber<Decimal128, 0>;

        //
        // Conversions from jstOID
        //
        table[BSONType::jstOID][BSONType::String] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.getOid().toString());
            };
        table[BSONType::jstOID][BSONType::jstOID] = &performIdentityConversion;
        table[BSONType::jstOID][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::jstOID][BSONType::Date] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.getOid().asDateT());
            };

        //
        // Conversions from Bool
        //
        table[BSONType::Bool][BSONType::NumberDouble] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(1.0) : Value(0.0);
            };
        table[BSONType::Bool][BSONType::String] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return inputValue.getBool() ? Value("true"_sd) : Value("false"_sd);
            };
        table[BSONType::Bool][BSONType::Bool] = &performIdentityConversion;
        table[BSONType::Bool][BSONType::NumberInt] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(int{1}) : Value(int{0});
            };
        table[BSONType::Bool][BSONType::NumberLong] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(1LL) : Value(0LL);
            };
        table[BSONType::Bool][BSONType::NumberDecimal] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(Decimal128(1)) : Value(Decimal128(0));
            };

        //
        // Conversions from Date
        //
        table[BSONType::Date][BSONType::NumberDouble] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(static_cast<double>(inputValue.getDate().toMillisSinceEpoch()));
            };
        table[BSONType::Date][BSONType::String] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                auto dateString = uassertStatusOK(TimeZoneDatabase::utcZone().formatDate(
                    Value::kISOFormatString, inputValue.getDate()));
                return Value(dateString);
            };
        table[BSONType::Date]
             [BSONType::Bool] = [](const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   Value inputValue) { return Value(inputValue.coerceToBool()); };
        table[BSONType::Date][BSONType::Date] = &performIdentityConversion;
        table[BSONType::Date][BSONType::NumberLong] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.getDate().toMillisSinceEpoch());
            };
        table[BSONType::Date][BSONType::NumberDecimal] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(
                    Decimal128(static_cast<int64_t>(inputValue.getDate().toMillisSinceEpoch())));
            };

        //
        // Conversions from NumberInt
        //
        table[BSONType::NumberInt][BSONType::NumberDouble] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.coerceToDouble());
            };
        table[BSONType::NumberInt][BSONType::String] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(static_cast<std::string>(str::stream() << inputValue.getInt()));
            };
        table[BSONType::NumberInt]
             [BSONType::Bool] = [](const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   Value inputValue) { return Value(inputValue.coerceToBool()); };
        table[BSONType::NumberInt][BSONType::NumberInt] = &performIdentityConversion;
        table[BSONType::NumberInt][BSONType::NumberLong] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(static_cast<long long>(inputValue.getInt()));
            };
        table[BSONType::NumberInt][BSONType::NumberDecimal] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.coerceToDecimal());
            };

        //
        // Conversions from NumberLong
        //
        table[BSONType::NumberLong][BSONType::NumberDouble] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.coerceToDouble());
            };
        table[BSONType::NumberLong][BSONType::String] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(static_cast<std::string>(str::stream() << inputValue.getLong()));
            };
        table[BSONType::NumberLong]
             [BSONType::Bool] = [](const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   Value inputValue) { return Value(inputValue.coerceToBool()); };
        table[BSONType::NumberLong][BSONType::Date] = &performCastNumberToDate;
        table[BSONType::NumberLong][BSONType::NumberInt] = &performCastLongToInt;
        table[BSONType::NumberLong][BSONType::NumberLong] = &performIdentityConversion;
        table[BSONType::NumberLong][BSONType::NumberDecimal] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.coerceToDecimal());
            };

        //
        // Conversions from NumberDecimal
        //
        table[BSONType::NumberDecimal][BSONType::NumberDouble] = &performCastDecimalToDouble;
        table[BSONType::NumberDecimal][BSONType::String] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return Value(inputValue.getDecimal().toString());
            };
        table[BSONType::NumberDecimal]
             [BSONType::Bool] = [](const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   Value inputValue) { return Value(inputValue.coerceToBool()); };
        table[BSONType::NumberDecimal][BSONType::Date] = &performCastNumberToDate;
        table[BSONType::NumberDecimal][BSONType::NumberInt] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return performCastDecimalToInt(BSONType::NumberInt, inputValue);
            };
        table[BSONType::NumberDecimal][BSONType::NumberLong] =
            [](const boost::intrusive_ptr<ExpressionContext>& expCtx, Value inputValue) {
                return performCastDecimalToInt(BSONType::NumberLong, inputValue);
            };
        table[BSONType::NumberDecimal][BSONType::NumberDecimal] = &performIdentityConversion;

        //
        // Miscellaneous conversions to Bool
        //
        table[BSONType::Object][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::Array][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::BinData][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::RegEx][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::DBRef][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::Code][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::Symbol][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::CodeWScope][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::bsonTimestamp][BSONType::Bool] = &performConvertToTrue;
    }

    ConversionFunc findConversionFunc(BSONType inputType, BSONType targetType) const {
        ConversionFunc foundFunction;

        // Note: We can't use BSONType::MinKey (-1) or BSONType::MaxKey (127) as table indexes,
        // so we have to treat them as special cases.
        if (inputType != BSONType::MinKey && inputType != BSONType::MaxKey &&
            targetType != BSONType::MinKey && targetType != BSONType::MaxKey) {
            invariant(inputType >= 0 && inputType <= JSTypeMax);
            invariant(targetType >= 0 && targetType <= JSTypeMax);
            foundFunction = table[inputType][targetType];
        } else if (targetType == BSONType::Bool) {
            // This is a conversion from MinKey or MaxKey to Bool, which is allowed (and always
            // returns true).
            foundFunction = &performConvertToTrue;
        } else {
            // Any other conversions involving MinKey or MaxKey (either as the target or input) are
            // illegal.
        }

        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Unsupported conversion from " << typeName(inputType) << " to "
                              << typeName(targetType)
                              << " in $convert with no onError value",
                foundFunction);
        return foundFunction;
    }

private:
    ConversionFunc table[JSTypeMax + 1][JSTypeMax + 1];

    static void validateDoubleValueIsFinite(double inputDouble) {
        uassert(ErrorCodes::ConversionFailure,
                "Attempt to convert NaN value to integer type in $convert with no onError value",
                !std::isnan(inputDouble));
        uassert(
            ErrorCodes::ConversionFailure,
            "Attempt to convert infinity value to integer type in $convert with no onError value",
            std::isfinite(inputDouble));
    }

    static Value performCastDoubleToInt(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Value inputValue) {
        double inputDouble = inputValue.getDouble();
        validateDoubleValueIsFinite(inputDouble);

        uassert(ErrorCodes::ConversionFailure,
                str::stream()
                    << "Conversion would overflow target type in $convert with no onError value: "
                    << inputDouble,
                inputDouble >= std::numeric_limits<int>::lowest() &&
                    inputDouble <= std::numeric_limits<int>::max());

        return Value(static_cast<int>(inputDouble));
    }

    static Value performCastDoubleToLong(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         Value inputValue) {
        double inputDouble = inputValue.getDouble();
        validateDoubleValueIsFinite(inputDouble);

        uassert(ErrorCodes::ConversionFailure,
                str::stream()
                    << "Conversion would overflow target type in $convert with no onError value: "
                    << inputDouble,
                inputDouble >= std::numeric_limits<long long>::lowest() &&
                    inputDouble < BSONElement::kLongLongMaxPlusOneAsDouble);

        return Value(static_cast<long long>(inputDouble));
    }

    static Value performCastDecimalToInt(BSONType targetType, Value inputValue) {
        invariant(targetType == BSONType::NumberInt || targetType == BSONType::NumberLong);
        Decimal128 inputDecimal = inputValue.getDecimal();

        // Performing these checks up front allows us to provide more specific error messages than
        // if we just gave the same error for any 'kInvalid' conversion.
        uassert(ErrorCodes::ConversionFailure,
                "Attempt to convert NaN value to integer type in $convert with no onError value",
                !inputDecimal.isNaN());
        uassert(
            ErrorCodes::ConversionFailure,
            "Attempt to convert infinity value to integer type in $convert with no onError value",
            !inputDecimal.isInfinite());

        std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
        Value result;
        if (targetType == BSONType::NumberInt) {
            int intVal =
                inputDecimal.toInt(&signalingFlags, Decimal128::RoundingMode::kRoundTowardZero);
            result = Value(intVal);
        } else if (targetType == BSONType::NumberLong) {
            long long longVal =
                inputDecimal.toLong(&signalingFlags, Decimal128::RoundingMode::kRoundTowardZero);
            result = Value(longVal);
        } else {
            MONGO_UNREACHABLE;
        }

        // NB: Decimal128::SignalingFlag has a values specifically for overflow, but it is used for
        // arithmetic with Decimal128 operands, _not_ for conversions of this style. Overflowing
        // conversions only trigger a 'kInvalid' flag.
        uassert(ErrorCodes::ConversionFailure,
                str::stream()
                    << "Conversion would overflow target type in $convert with no onError value: "
                    << inputDecimal.toString(),
                (signalingFlags & Decimal128::SignalingFlag::kInvalid) == 0);
        invariant(signalingFlags == Decimal128::SignalingFlag::kNoFlag);

        return result;
    }

    static Value performCastDecimalToDouble(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            Value inputValue) {
        Decimal128 inputDecimal = inputValue.getDecimal();

        std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
        double result =
            inputDecimal.toDouble(&signalingFlags, Decimal128::RoundingMode::kRoundTiesToEven);

        uassert(ErrorCodes::ConversionFailure,
                str::stream()
                    << "Conversion would overflow target type in $convert with no onError value: "
                    << inputDecimal.toString(),
                signalingFlags == Decimal128::SignalingFlag::kNoFlag ||
                    signalingFlags == Decimal128::SignalingFlag::kInexact);

        return Value(result);
    }

    static Value performCastLongToInt(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      Value inputValue) {
        long long longValue = inputValue.getLong();

        uassert(ErrorCodes::ConversionFailure,
                str::stream()
                    << "Conversion would overflow target type in $convert with no onError value: ",
                longValue >= std::numeric_limits<int>::min() &&
                    longValue <= std::numeric_limits<int>::max());

        return Value(static_cast<int>(longValue));
    }

    static Value performCastNumberToDate(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         Value inputValue) {
        long long millisSinceEpoch;

        switch (inputValue.getType()) {
            case BSONType::NumberLong:
                millisSinceEpoch = inputValue.getLong();
                break;
            case BSONType::NumberDouble:
                millisSinceEpoch = performCastDoubleToLong(expCtx, inputValue).getLong();
                break;
            case BSONType::NumberDecimal:
                millisSinceEpoch =
                    performCastDecimalToInt(BSONType::NumberLong, inputValue).getLong();
                break;
            default:
                MONGO_UNREACHABLE;
        }

        return Value(Date_t::fromMillisSinceEpoch(millisSinceEpoch));
    }

    static Value performFormatDouble(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     Value inputValue) {
        double doubleValue = inputValue.getDouble();

        if (std::isinf(doubleValue)) {
            return Value(std::signbit(doubleValue) ? "-Infinity"_sd : "Infinity"_sd);
        } else if (std::isnan(doubleValue)) {
            return Value("NaN"_sd);
        } else if (doubleValue == 0.0 && std::signbit(doubleValue)) {
            return Value("-0"_sd);
        } else {
            return Value(static_cast<std::string>(str::stream() << doubleValue));
        }
    }

    template <class targetType, int base>
    static Value parseStringToNumber(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     Value inputValue) {
        auto stringValue = inputValue.getStringData();
        targetType result;

        // Reject any strings in hex format. This check is needed because the
        // parseNumberFromStringWithBase call below allows an input hex string prefixed by '0x' when
        // parsing to a double.
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Illegal hexadecimal input in $convert with no onError value: "
                              << stringValue,
                !stringValue.startsWith("0x"));

        Status parseStatus = parseNumberFromStringWithBase(stringValue, base, &result);
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Failed to parse number '" << stringValue
                              << "' in $convert with no onError value: "
                              << parseStatus.reason(),
                parseStatus.isOK());

        return Value(result);
    }

    static Value parseStringToOID(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  Value inputValue) {
        try {
            return Value(OID::createFromString(inputValue.getStringData()));
        } catch (const DBException& ex) {
            // Rethrow any caught exception as a conversion failure such that 'onError' is evaluated
            // and returned.
            uasserted(ErrorCodes::ConversionFailure,
                      str::stream() << "Failed to parse objectId '" << inputValue.getString()
                                    << "' in $convert with no onError value: "
                                    << ex.reason());
        }
    }

    static Value performConvertToTrue(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      Value inputValue) {
        return Value(true);
    }

    static Value performIdentityConversion(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           Value inputValue) {
        return inputValue;
    }
};

Expression::Parser makeConversionAlias(const StringData shortcutName, BSONType toType) {
    return [=](const intrusive_ptr<ExpressionContext>& expCtx,
               BSONElement elem,
               const VariablesParseState& vps) -> intrusive_ptr<Expression> {

        // Use parseArguments to allow for a singleton array, or the unwrapped version.
        auto operands = ExpressionNary::parseArguments(expCtx, elem, vps);

        uassert(50723,
                str::stream() << shortcutName << " requires a single argument, got "
                              << operands.size(),
                operands.size() == 1);
        return ExpressionConvert::create(expCtx, operands[0], toType);
    };
}

}  // namespace

REGISTER_EXPRESSION_WITH_MIN_VERSION(
    convert,
    ExpressionConvert::parse,
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

// Also register shortcut expressions like $toInt, $toString, etc. which can be used as a shortcut
// for $convert without an 'onNull' or 'onError'.
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toString,
    makeConversionAlias("$toString"_sd, BSONType::String),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toObjectId,
    makeConversionAlias("$toObjectId"_sd, BSONType::jstOID),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toDate,
    makeConversionAlias("$toDate"_sd, BSONType::Date),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toDouble,
    makeConversionAlias("$toDouble"_sd, BSONType::NumberDouble),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toInt,
    makeConversionAlias("$toInt"_sd, BSONType::NumberInt),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toLong,
    makeConversionAlias("$toLong"_sd, BSONType::NumberLong),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toDecimal,
    makeConversionAlias("$toDecimal"_sd, BSONType::NumberDecimal),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
REGISTER_EXPRESSION_WITH_MIN_VERSION(
    toBool,
    makeConversionAlias("$toBool"_sd, BSONType::Bool),
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

boost::intrusive_ptr<Expression> ExpressionConvert::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& input,
    BSONType toType) {
    return new ExpressionConvert(expCtx, input, toType);
}

ExpressionConvert::ExpressionConvert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const boost::intrusive_ptr<Expression>& input,
                                     BSONType toType)
    : Expression(expCtx),
      _input(input),
      _to(ExpressionConstant::create(expCtx, Value(StringData(typeName(toType))))) {}

intrusive_ptr<Expression> ExpressionConvert::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$convert expects an object of named arguments but found: "
                          << typeName(expr.type()),
            expr.type() == BSONType::Object);

    intrusive_ptr<ExpressionConvert> newConvert(new ExpressionConvert(expCtx));

    for (auto&& elem : expr.embeddedObject()) {
        const auto field = elem.fieldNameStringData();
        if (field == "input"_sd) {
            newConvert->_input = parseOperand(expCtx, elem, vps);
        } else if (field == "to"_sd) {
            newConvert->_to = parseOperand(expCtx, elem, vps);
        } else if (field == "onError"_sd) {
            newConvert->_onError = parseOperand(expCtx, elem, vps);
        } else if (field == "onNull"_sd) {
            newConvert->_onNull = parseOperand(expCtx, elem, vps);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "$convert found an unknown argument: "
                                    << elem.fieldNameStringData());
        }
    }

    uassert(ErrorCodes::FailedToParse, "Missing 'input' parameter to $convert", newConvert->_input);
    uassert(ErrorCodes::FailedToParse, "Missing 'to' parameter to $convert", newConvert->_to);

    return std::move(newConvert);
}

Value ExpressionConvert::evaluate(const Document& root, Variables* variables) const {
    auto toValue = _to->evaluate(root, variables);
    Value inputValue = _input->evaluate(root, variables);
    boost::optional<BSONType> targetType;
    if (!toValue.nullish()) {
        targetType = computeTargetType(toValue);
    }

    if (inputValue.nullish()) {
        return _onNull ? _onNull->evaluate(root, variables) : Value(BSONNULL);
    } else if (!targetType) {
        // "to" evaluated to a nullish value.
        return Value(BSONNULL);
    }

    try {
        return performConversion(*targetType, inputValue);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (_onError) {
            return _onError->evaluate(root, variables);
        } else {
            throw;
        }
    }
}

boost::intrusive_ptr<Expression> ExpressionConvert::optimize() {
    _input = _input->optimize();
    _to = _to->optimize();
    if (_onError) {
        _onError = _onError->optimize();
    }
    if (_onNull) {
        _onNull = _onNull->optimize();
    }

    // Perform constant folding if possible. This does not support folding for $convert operations
    // that have constant _to and _input values but non-constant _onError and _onNull values.
    // Because _onError and _onNull are evaluated lazily, conversions that do not used the _onError
    // and _onNull values could still be legally folded if those values are not needed. Support for
    // that case would add more complexity than it's worth, though.
    if (ExpressionConstant::allNullOrConstant({_input, _to, _onError, _onNull})) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }

    return this;
}

Value ExpressionConvert::serialize(bool explain) const {
    return Value(Document{{"$convert",
                           Document{{"input", _input->serialize(explain)},
                                    {"to", _to->serialize(explain)},
                                    {"onError", _onError ? _onError->serialize(explain) : Value()},
                                    {"onNull", _onNull ? _onNull->serialize(explain) : Value()}}}});
}

void ExpressionConvert::_doAddDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    _to->addDependencies(deps);
    if (_onError) {
        _onError->addDependencies(deps);
    }
    if (_onNull) {
        _onNull->addDependencies(deps);
    }
}

BSONType ExpressionConvert::computeTargetType(Value targetTypeName) const {
    BSONType targetType;
    if (targetTypeName.getType() == BSONType::String) {
        // typeFromName() does not consider "missing" to be a valid type, but we want to accept it,
        // because it is a possible result of the $type aggregation operator.
        if (targetTypeName.getStringData() == "missing"_sd) {
            return BSONType::EOO;
        }

        // This will throw if the type name is invalid.
        targetType = typeFromName(targetTypeName.getString());
    } else if (targetTypeName.numeric()) {
        uassert(ErrorCodes::FailedToParse,
                "In $convert, numeric 'to' argument is not an integer",
                targetTypeName.integral());

        int typeCode = targetTypeName.coerceToInt();
        uassert(ErrorCodes::FailedToParse,
                str::stream()
                    << "In $convert, numeric value for 'to' does not correspond to a BSON type: "
                    << typeCode,
                isValidBSONType(typeCode));
        targetType = static_cast<BSONType>(typeCode);
    } else {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "$convert's 'to' argument must be a string or number, but is "
                                << typeName(targetTypeName.getType()));
    }

    return targetType;
}

Value ExpressionConvert::performConversion(BSONType targetType, Value inputValue) const {
    invariant(!inputValue.nullish());

    static const ConversionTable table;
    BSONType inputType = inputValue.getType();
    return table.findConversionFunc(inputType, targetType)(getExpressionContext(), inputValue);
}

}  // namespace mongo
