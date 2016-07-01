/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/expression.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cstdio>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
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
    return Value(DOC("$const" << val));
}

void Variables::uassertValidNameForUserWrite(StringData varName) {
    // System variables users allowed to write to (currently just one)
    if (varName == "CURRENT") {
        return;
    }

    uassert(16866, "empty variable names are not allowed", !varName.empty());

    const bool firstCharIsValid =
        (varName[0] >= 'a' && varName[0] <= 'z') || (varName[0] & '\x80')  // non-ascii
        ;

    uassert(16867,
            str::stream() << "'" << varName
                          << "' starts with an invalid character for a user variable name",
            firstCharIsValid);

    for (size_t i = 1; i < varName.size(); i++) {
        const bool charIsValid = (varName[i] >= 'a' && varName[i] <= 'z') ||
            (varName[i] >= 'A' && varName[i] <= 'Z') || (varName[i] >= '0' && varName[i] <= '9') ||
            (varName[i] == '_') || (varName[i] & '\x80')  // non-ascii
            ;

        uassert(16868,
                str::stream() << "'" << varName << "' contains an invalid character "
                              << "for a variable name: '"
                              << varName[i]
                              << "'",
                charIsValid);
    }
}

void Variables::uassertValidNameForUserRead(StringData varName) {
    uassert(16869, "empty variable names are not allowed", !varName.empty());

    const bool firstCharIsValid = (varName[0] >= 'a' && varName[0] <= 'z') ||
        (varName[0] >= 'A' && varName[0] <= 'Z') || (varName[0] & '\x80')  // non-ascii
        ;

    uassert(16870,
            str::stream() << "'" << varName
                          << "' starts with an invalid character for a variable name",
            firstCharIsValid);

    for (size_t i = 1; i < varName.size(); i++) {
        const bool charIsValid = (varName[i] >= 'a' && varName[i] <= 'z') ||
            (varName[i] >= 'A' && varName[i] <= 'Z') || (varName[i] >= '0' && varName[i] <= '9') ||
            (varName[i] == '_') || (varName[i] & '\x80')  // non-ascii
            ;

        uassert(16871,
                str::stream() << "'" << varName << "' contains an invalid character "
                              << "for a variable name: '"
                              << varName[i]
                              << "'",
                charIsValid);
    }
}

void Variables::setValue(Id id, const Value& value) {
    massert(17199, "can't use Variables::setValue to set ROOT", id != ROOT_ID);

    verify(id < _numVars);
    _rest[id] = value;
}

Value Variables::getValue(Id id) const {
    if (id == ROOT_ID)
        return Value(_root);

    verify(id < _numVars);
    return _rest[id];
}

Document Variables::getDocument(Id id) const {
    if (id == ROOT_ID)
        return _root;

    verify(id < _numVars);
    const Value var = _rest[id];
    if (var.getType() == Object)
        return var.getDocument();

    return Document();
}

Variables::Id VariablesParseState::defineVariable(StringData name) {
    // caller should have validated before hand by using Variables::uassertValidNameForUserWrite
    massert(17275, "Can't redefine ROOT", name != "ROOT");

    Variables::Id id = _idGenerator->generateId();
    _variables[name] = id;
    return id;
}

Variables::Id VariablesParseState::getVariable(StringData name) const {
    StringMap<Variables::Id>::const_iterator it = _variables.find(name);
    if (it != _variables.end())
        return it->second;

    uassert(17276,
            str::stream() << "Use of undefined variable: " << name,
            name == "ROOT" || name == "CURRENT");

    return Variables::ROOT_ID;
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

intrusive_ptr<Expression> Expression::parseObject(BSONObj obj, const VariablesParseState& vps) {
    if (obj.isEmpty()) {
        return ExpressionObject::create({});
    }

    if (obj.firstElementFieldName()[0] == '$') {
        // Assume this is an expression like {$add: [...]}.
        return parseExpression(obj, vps);
    }

    return ExpressionObject::parse(obj, vps);
}

namespace {
StringMap<Parser> parserMap;
}

void Expression::registerExpression(string key, Parser parser) {
    auto op = parserMap.find(key);
    massert(17064,
            str::stream() << "Duplicate expression (" << key << ") registered.",
            op == parserMap.end());
    parserMap[key] = parser;
}

intrusive_ptr<Expression> Expression::parseExpression(BSONObj obj, const VariablesParseState& vps) {
    uassert(15983,
            str::stream() << "An object representing an expression must have exactly one "
                             "field: "
                          << obj.toString(),
            obj.nFields() == 1);

    // Look up the parser associated with the expression name.
    const char* opName = obj.firstElementFieldName();
    auto op = parserMap.find(opName);
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Unrecognized expression '" << opName << "'",
            op != parserMap.end());
    return op->second(obj.firstElement(), vps);
}

Expression::ExpressionVector ExpressionNary::parseArguments(BSONElement exprElement,
                                                            const VariablesParseState& vps) {
    ExpressionVector out;
    if (exprElement.type() == Array) {
        BSONForEach(elem, exprElement.Obj()) {
            out.push_back(Expression::parseOperand(elem, vps));
        }
    } else {  // Assume it's an operand that accepts a single argument.
        out.push_back(Expression::parseOperand(exprElement, vps));
    }

    return out;
}

intrusive_ptr<Expression> Expression::parseOperand(BSONElement exprElement,
                                                   const VariablesParseState& vps) {
    BSONType type = exprElement.type();

    if (type == String && exprElement.valuestr()[0] == '$') {
        /* if we got here, this is a field path expression */
        return ExpressionFieldPath::parse(exprElement.str(), vps);
    } else if (type == Object) {
        return Expression::parseObject(exprElement.Obj(), vps);
    } else if (type == Array) {
        return ExpressionArray::parse(exprElement, vps);
    } else {
        return ExpressionConstant::parse(exprElement, vps);
    }
}

namespace {
/**
 * UTF-8 multi-byte code points consist of one leading byte of the form 11xxxxxx, and potentially
 * many continuation bytes of the form 10xxxxxx. This method checks whether 'charByte' is a
 * continuation byte.
 */
bool isContinuationByte(char charByte) {
    return (charByte & 0xc0) == 0x80;
}

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

Value ExpressionAdd::evaluateInternal(Variables* vars) const {
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
        Value val = vpOperand[i]->evaluateInternal(vars);

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
                nonDecimalTotal.addLong(val.getDate());
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

Value ExpressionAllElementsTrue::evaluateInternal(Variables* vars) const {
    const Value arr = vpOperand[0]->evaluateInternal(vars);
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

Value ExpressionAnd::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(vpOperand[i]->evaluateInternal(vars));
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

Value ExpressionAnyElementTrue::evaluateInternal(Variables* vars) const {
    const Value arr = vpOperand[0]->evaluateInternal(vars);
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

Value ExpressionArray::evaluateInternal(Variables* vars) const {
    vector<Value> values;
    values.reserve(vpOperand.size());
    for (auto&& expr : vpOperand) {
        Value elemVal = expr->evaluateInternal(vars);
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

const char* ExpressionArray::getOpName() const {
    // This should never be called, but is needed to inherit from ExpressionNary.
    return "$array";
}

/* ------------------------- ExpressionArrayElemAt -------------------------- */

Value ExpressionArrayElemAt::evaluateInternal(Variables* vars) const {
    const Value array = vpOperand[0]->evaluateInternal(vars);
    const Value indexArg = vpOperand[1]->evaluateInternal(vars);

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
    intrusive_ptr<ExpressionCoerceToBool> pNew(new ExpressionCoerceToBool(pExpression));
    pNew->injectExpressionContext(expCtx);
    return pNew;
}

ExpressionCoerceToBool::ExpressionCoerceToBool(const intrusive_ptr<Expression>& pTheExpression)
    : Expression(), pExpression(pTheExpression) {}

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

void ExpressionCoerceToBool::addDependencies(DepsTracker* deps) const {
    pExpression->addDependencies(deps);
}

Value ExpressionCoerceToBool::evaluateInternal(Variables* vars) const {
    Value pResult(pExpression->evaluateInternal(vars));
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

void ExpressionCoerceToBool::doInjectExpressionContext() {
    // Inject our ExpressionContext into the operand.
    pExpression->injectExpressionContext(getExpressionContext());
}

/* ----------------------- ExpressionCompare --------------------------- */

REGISTER_EXPRESSION(cmp,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::CMP));
REGISTER_EXPRESSION(eq,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::EQ));
REGISTER_EXPRESSION(gt,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::GT));
REGISTER_EXPRESSION(gte,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::GTE));
REGISTER_EXPRESSION(lt,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::LT));
REGISTER_EXPRESSION(lte,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::LTE));
REGISTER_EXPRESSION(ne,
                    stdx::bind(ExpressionCompare::parse,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               ExpressionCompare::NE));
intrusive_ptr<Expression> ExpressionCompare::parse(BSONElement bsonExpr,
                                                   const VariablesParseState& vps,
                                                   CmpOp op) {
    intrusive_ptr<ExpressionCompare> expr = new ExpressionCompare(op);
    ExpressionVector args = parseArguments(bsonExpr, vps);
    expr->validateArguments(args);
    expr->vpOperand = args;
    return expr;
}

ExpressionCompare::ExpressionCompare(CmpOp theCmpOp) : cmpOp(theCmpOp) {}

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

Value ExpressionCompare::evaluateInternal(Variables* vars) const {
    Value pLeft(vpOperand[0]->evaluateInternal(vars));
    Value pRight(vpOperand[1]->evaluateInternal(vars));

    int cmp = Value::compare(pLeft, pRight);

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

Value ExpressionConcat::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();

    StringBuilder result;
    for (size_t i = 0; i < n; ++i) {
        Value val = vpOperand[i]->evaluateInternal(vars);
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

Value ExpressionConcatArrays::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();
    vector<Value> values;

    for (size_t i = 0; i < n; ++i) {
        Value val = vpOperand[i]->evaluateInternal(vars);
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

Value ExpressionCond::evaluateInternal(Variables* vars) const {
    Value pCond(vpOperand[0]->evaluateInternal(vars));
    int idx = pCond.coerceToBool() ? 1 : 2;
    return vpOperand[idx]->evaluateInternal(vars);
}

intrusive_ptr<Expression> ExpressionCond::parse(BSONElement expr, const VariablesParseState& vps) {
    if (expr.type() != Object) {
        return Base::parse(expr, vps);
    }
    verify(str::equals(expr.fieldName(), "$cond"));

    intrusive_ptr<ExpressionCond> ret = new ExpressionCond();
    ret->vpOperand.resize(3);

    const BSONObj args = expr.embeddedObject();
    BSONForEach(arg, args) {
        if (str::equals(arg.fieldName(), "if")) {
            ret->vpOperand[0] = parseOperand(arg, vps);
        } else if (str::equals(arg.fieldName(), "then")) {
            ret->vpOperand[1] = parseOperand(arg, vps);
        } else if (str::equals(arg.fieldName(), "else")) {
            ret->vpOperand[2] = parseOperand(arg, vps);
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

intrusive_ptr<Expression> ExpressionConstant::parse(BSONElement exprElement,
                                                    const VariablesParseState& vps) {
    return new ExpressionConstant(Value(exprElement));
}


intrusive_ptr<ExpressionConstant> ExpressionConstant::create(
    const intrusive_ptr<ExpressionContext>& expCtx, const Value& pValue) {
    intrusive_ptr<ExpressionConstant> pEC(new ExpressionConstant(pValue));
    pEC->injectExpressionContext(expCtx);
    return pEC;
}

ExpressionConstant::ExpressionConstant(const Value& pTheValue) : pValue(pTheValue) {}


intrusive_ptr<Expression> ExpressionConstant::optimize() {
    /* nothing to do */
    return intrusive_ptr<Expression>(this);
}

void ExpressionConstant::addDependencies(DepsTracker* deps) const {
    /* nothing to do */
}

Value ExpressionConstant::evaluateInternal(Variables* vars) const {
    return pValue;
}

Value ExpressionConstant::serialize(bool explain) const {
    return serializeConstant(pValue);
}

REGISTER_EXPRESSION(const, ExpressionConstant::parse);
REGISTER_EXPRESSION(literal, ExpressionConstant::parse);  // alias
const char* ExpressionConstant::getOpName() const {
    return "$const";
}

/* ---------------------- ExpressionDateToString ----------------------- */

REGISTER_EXPRESSION(dateToString, ExpressionDateToString::parse);
intrusive_ptr<Expression> ExpressionDateToString::parse(BSONElement expr,
                                                        const VariablesParseState& vps) {
    verify(str::equals(expr.fieldName(), "$dateToString"));

    uassert(18629, "$dateToString only supports an object as its argument", expr.type() == Object);

    BSONElement formatElem;
    BSONElement dateElem;
    const BSONObj args = expr.embeddedObject();
    BSONForEach(arg, args) {
        if (str::equals(arg.fieldName(), "format")) {
            formatElem = arg;
        } else if (str::equals(arg.fieldName(), "date")) {
            dateElem = arg;
        } else {
            uasserted(18534,
                      str::stream() << "Unrecognized argument to $dateToString: "
                                    << arg.fieldName());
        }
    }

    uassert(18627, "Missing 'format' parameter to $dateToString", !formatElem.eoo());
    uassert(18628, "Missing 'date' parameter to $dateToString", !dateElem.eoo());

    uassert(18533,
            "The 'format' parameter to $dateToString must be a string literal",
            formatElem.type() == String);

    const string format = formatElem.str();

    validateFormat(format);

    return new ExpressionDateToString(format, parseOperand(dateElem, vps));
}

ExpressionDateToString::ExpressionDateToString(const string& format, intrusive_ptr<Expression> date)
    : _format(format), _date(date) {}

intrusive_ptr<Expression> ExpressionDateToString::optimize() {
    _date = _date->optimize();
    return this;
}

Value ExpressionDateToString::serialize(bool explain) const {
    return Value(
        DOC("$dateToString" << DOC("format" << _format << "date" << _date->serialize(explain))));
}

Value ExpressionDateToString::evaluateInternal(Variables* vars) const {
    const Value date = _date->evaluateInternal(vars);

    if (date.nullish()) {
        return Value(BSONNULL);
    }

    return Value(formatDate(_format, date.coerceToTm(), date.coerceToDate()));
}

// verifies that any '%' is followed by a valid format character, and that
// the format string ends with an even number of '%' symbols
void ExpressionDateToString::validateFormat(const std::string& format) {
    for (string::const_iterator it = format.begin(); it != format.end(); ++it) {
        if (*it != '%') {
            continue;
        }

        ++it;  // next character must be format modifier
        uassert(18535, "Unmatched '%' at end of $dateToString format string", it != format.end());


        switch (*it) {
            // all of these fall through intentionally
            case '%':
            case 'Y':
            case 'm':
            case 'd':
            case 'H':
            case 'M':
            case 'S':
            case 'L':
            case 'j':
            case 'w':
            case 'U':
            case 'G':
            case 'V':
            case 'u':
                break;
            default:
                uasserted(18536,
                          str::stream() << "Invalid format character '%" << *it
                                        << "' in $dateToString format string");
        }
    }
}

string ExpressionDateToString::formatDate(const string& format,
                                          const tm& tm,
                                          const long long date) {
    StringBuilder formatted;
    for (string::const_iterator it = format.begin(); it != format.end(); ++it) {
        if (*it != '%') {
            formatted << *it;
            continue;
        }

        ++it;                           // next character is format modifier
        invariant(it != format.end());  // checked in validateFormat

        switch (*it) {
            case '%':  // Escaped literal %
                formatted << '%';
                break;
            case 'Y':  // Year
            {
                const int year = ExpressionYear::extract(tm);
                uassert(18537,
                        str::stream() << "$dateToString is only defined on year 0-9999,"
                                      << " tried to use year "
                                      << year,
                        (year >= 0) && (year <= 9999));
                insertPadded(formatted, year, 4);
                break;
            }
            case 'm':  // Month
                insertPadded(formatted, ExpressionMonth::extract(tm), 2);
                break;
            case 'd':  // Day of month
                insertPadded(formatted, ExpressionDayOfMonth::extract(tm), 2);
                break;
            case 'H':  // Hour
                insertPadded(formatted, ExpressionHour::extract(tm), 2);
                break;
            case 'M':  // Minute
                insertPadded(formatted, ExpressionMinute::extract(tm), 2);
                break;
            case 'S':  // Second
                insertPadded(formatted, ExpressionSecond::extract(tm), 2);
                break;
            case 'L':  // Millisecond
                insertPadded(formatted, ExpressionMillisecond::extract(date), 3);
                break;
            case 'j':  // Day of year
                insertPadded(formatted, ExpressionDayOfYear::extract(tm), 3);
                break;
            case 'w':  // Day of week
                insertPadded(formatted, ExpressionDayOfWeek::extract(tm), 1);
                break;
            case 'U':  // Week
                insertPadded(formatted, ExpressionWeek::extract(tm), 2);
                break;
            case 'G':  // Iso year of week
                insertPadded(formatted, ExpressionIsoWeekYear::extract(tm), 4);
                break;
            case 'V':  // Iso week
                insertPadded(formatted, ExpressionIsoWeek::extract(tm), 2);
                break;
            case 'u':  // Iso day of week
                insertPadded(formatted, ExpressionIsoDayOfWeek::extract(tm), 1);
                break;
            default:
                // Should never happen as format is pre-validated
                invariant(false);
        }
    }
    return formatted.str();
}

// Only works with 1 <= spaces <= 4 and 0 <= number <= 9999.
// If spaces is less than the digit count of number we simply insert the number
// without padding.
void ExpressionDateToString::insertPadded(StringBuilder& sb, int number, int width) {
    invariant(width >= 1);
    invariant(width <= 4);
    invariant(number >= 0);
    invariant(number <= 9999);

    int digits = 1;

    if (number >= 1000) {
        digits = 4;
    } else if (number >= 100) {
        digits = 3;
    } else if (number >= 10) {
        digits = 2;
    }

    if (width > digits) {
        sb.write("0000", width - digits);
    }
    sb << number;
}

void ExpressionDateToString::addDependencies(DepsTracker* deps) const {
    _date->addDependencies(deps);
}

void ExpressionDateToString::doInjectExpressionContext() {
    _date->injectExpressionContext(getExpressionContext());
}

/* ---------------------- ExpressionDayOfMonth ------------------------- */

Value ExpressionDayOfMonth::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(dayOfMonth, ExpressionDayOfMonth::parse);
const char* ExpressionDayOfMonth::getOpName() const {
    return "$dayOfMonth";
}

/* ------------------------- ExpressionDayOfWeek ----------------------------- */

Value ExpressionDayOfWeek::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(dayOfWeek, ExpressionDayOfWeek::parse);
const char* ExpressionDayOfWeek::getOpName() const {
    return "$dayOfWeek";
}

/* ------------------------- ExpressionDayOfYear ----------------------------- */

Value ExpressionDayOfYear::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(dayOfYear, ExpressionDayOfYear::parse);
const char* ExpressionDayOfYear::getOpName() const {
    return "$dayOfYear";
}

/* ----------------------- ExpressionDivide ---------------------------- */

Value ExpressionDivide::evaluateInternal(Variables* vars) const {
    Value lhs = vpOperand[0]->evaluateInternal(vars);
    Value rhs = vpOperand[1]->evaluateInternal(vars);

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

ExpressionObject::ExpressionObject(vector<pair<string, intrusive_ptr<Expression>>>&& expressions)
    : _expressions(std::move(expressions)) {}

intrusive_ptr<ExpressionObject> ExpressionObject::create(
    vector<pair<string, intrusive_ptr<Expression>>>&& expressions) {
    return new ExpressionObject(std::move(expressions));
}

intrusive_ptr<ExpressionObject> ExpressionObject::parse(BSONObj obj,
                                                        const VariablesParseState& vps) {
    // Make sure we don't have any duplicate field names.
    std::unordered_set<string> specifiedFields;

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
        expressions.emplace_back(fieldName, parseOperand(elem, vps));
    }

    return new ExpressionObject{std::move(expressions)};
}

intrusive_ptr<Expression> ExpressionObject::optimize() {
    for (auto&& pair : _expressions) {
        pair.second = pair.second->optimize();
    }
    return this;
}

void ExpressionObject::addDependencies(DepsTracker* deps) const {
    for (auto&& pair : _expressions) {
        pair.second->addDependencies(deps);
    }
}

Value ExpressionObject::evaluateInternal(Variables* vars) const {
    MutableDocument outputDoc;
    for (auto&& pair : _expressions) {
        outputDoc.setNestedField(FieldPath(pair.first), pair.second->evaluateInternal(vars));
    }
    return outputDoc.freezeToValue();
}

Value ExpressionObject::serialize(bool explain) const {
    MutableDocument outputDoc;
    for (auto&& pair : _expressions) {
        outputDoc.setNestedField(FieldPath(pair.first), pair.second->serialize(explain));
    }
    return outputDoc.freezeToValue();
}

void ExpressionObject::doInjectExpressionContext() {
    for (auto&& pair : _expressions) {
        pair.second->injectExpressionContext(getExpressionContext());
    }
}

/* --------------------- ExpressionFieldPath --------------------------- */

// this is the old deprecated version only used by tests not using variables
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::create(const string& fieldPath) {
    return new ExpressionFieldPath("CURRENT." + fieldPath, Variables::ROOT_ID);
}

// this is the new version that supports every syntax
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::parse(const string& raw,
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
        return new ExpressionFieldPath(fieldPath.toString(), vps.getVariable(varName));
    } else {
        return new ExpressionFieldPath("CURRENT." + raw.substr(1),  // strip the "$" prefix
                                       vps.getVariable("CURRENT"));
    }
}


ExpressionFieldPath::ExpressionFieldPath(const string& theFieldPath, Variables::Id variable)
    : _fieldPath(theFieldPath), _variable(variable) {}

intrusive_ptr<Expression> ExpressionFieldPath::optimize() {
    /* nothing can be done for these */
    return intrusive_ptr<Expression>(this);
}

void ExpressionFieldPath::addDependencies(DepsTracker* deps) const {
    if (_variable == Variables::ROOT_ID) {  // includes CURRENT when it is equivalent to ROOT.
        if (_fieldPath.getPathLength() == 1) {
            deps->needWholeDocument = true;  // need full doc if just "$$ROOT"
        } else {
            deps->fields.insert(_fieldPath.tail().fullPath());
        }
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

Value ExpressionFieldPath::evaluateInternal(Variables* vars) const {
    if (_fieldPath.getPathLength() == 1)  // get the whole variable
        return vars->getValue(_variable);

    if (_variable == Variables::ROOT_ID) {
        // ROOT is always a document so use optimized code path
        return evaluatePath(1, vars->getRoot());
    }

    Value var = vars->getValue(_variable);
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

/* ------------------------- ExpressionFilter ----------------------------- */

REGISTER_EXPRESSION(filter, ExpressionFilter::parse);
intrusive_ptr<Expression> ExpressionFilter::parse(BSONElement expr,
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
    intrusive_ptr<Expression> input = parseOperand(inputElem, vpsIn);

    // Parse "as".
    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our variable, vpsIn doesn't.

    // If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    Variables::uassertValidNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // Parse "cond", has access to "as" variable.
    intrusive_ptr<Expression> cond = parseOperand(condElem, vpsSub);

    return new ExpressionFilter(std::move(varName), varId, std::move(input), std::move(cond));
}

ExpressionFilter::ExpressionFilter(string varName,
                                   Variables::Id varId,
                                   intrusive_ptr<Expression> input,
                                   intrusive_ptr<Expression> filter)
    : _varName(std::move(varName)),
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

Value ExpressionFilter::evaluateInternal(Variables* vars) const {
    // We are guaranteed at parse time that this isn't using our _varId.
    const Value inputVal = _input->evaluateInternal(vars);
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
        vars->setValue(_varId, elem);

        if (_filter->evaluateInternal(vars).coerceToBool()) {
            output.push_back(std::move(elem));
        }
    }

    return Value(std::move(output));
}

void ExpressionFilter::addDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    _filter->addDependencies(deps);
}

void ExpressionFilter::doInjectExpressionContext() {
    _input->injectExpressionContext(getExpressionContext());
    _filter->injectExpressionContext(getExpressionContext());
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
intrusive_ptr<Expression> ExpressionLet::parse(BSONElement expr, const VariablesParseState& vpsIn) {
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

        vars[id] = NameAndExpression(varName, parseOperand(varElem, vpsIn));  // only has outer vars
    }

    // parse "in"
    intrusive_ptr<Expression> subExpression = parseOperand(inElem, vpsSub);  // has our vars

    return new ExpressionLet(vars, subExpression);
}

ExpressionLet::ExpressionLet(const VariableMap& vars, intrusive_ptr<Expression> subExpression)
    : _variables(vars), _subExpression(subExpression) {}

intrusive_ptr<Expression> ExpressionLet::optimize() {
    if (_variables.empty()) {
        // we aren't binding any variables so just return the subexpression
        return _subExpression->optimize();
    }

    for (VariableMap::iterator it = _variables.begin(), end = _variables.end(); it != end; ++it) {
        it->second.expression = it->second.expression->optimize();
    }

    // TODO be smarter with constant "variables"
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

Value ExpressionLet::evaluateInternal(Variables* vars) const {
    for (VariableMap::const_iterator it = _variables.begin(), end = _variables.end(); it != end;
         ++it) {
        // It is guaranteed at parse-time that these expressions don't use the variable ids we
        // are setting
        vars->setValue(it->first, it->second.expression->evaluateInternal(vars));
    }

    return _subExpression->evaluateInternal(vars);
}

void ExpressionLet::addDependencies(DepsTracker* deps) const {
    for (VariableMap::const_iterator it = _variables.begin(), end = _variables.end(); it != end;
         ++it) {
        it->second.expression->addDependencies(deps);
    }

    // TODO be smarter when CURRENT is a bound variable
    _subExpression->addDependencies(deps);
}

void ExpressionLet::doInjectExpressionContext() {
    _subExpression->injectExpressionContext(getExpressionContext());
}


/* ------------------------- ExpressionMap ----------------------------- */

REGISTER_EXPRESSION(map, ExpressionMap::parse);
intrusive_ptr<Expression> ExpressionMap::parse(BSONElement expr, const VariablesParseState& vpsIn) {
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
    intrusive_ptr<Expression> input = parseOperand(inputElem, vpsIn);  // only has outer vars

    // parse "as"
    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our vars, vpsIn doesn't.

    // If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    Variables::uassertValidNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // parse "in"
    intrusive_ptr<Expression> in = parseOperand(inElem, vpsSub);  // has access to map variable

    return new ExpressionMap(varName, varId, input, in);
}

ExpressionMap::ExpressionMap(const string& varName,
                             Variables::Id varId,
                             intrusive_ptr<Expression> input,
                             intrusive_ptr<Expression> each)
    : _varName(varName), _varId(varId), _input(input), _each(each) {}

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

Value ExpressionMap::evaluateInternal(Variables* vars) const {
    // guaranteed at parse time that this isn't using our _varId
    const Value inputVal = _input->evaluateInternal(vars);
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
        vars->setValue(_varId, input[i]);

        Value toInsert = _each->evaluateInternal(vars);
        if (toInsert.missing())
            toInsert = Value(BSONNULL);  // can't insert missing values into array

        output.push_back(toInsert);
    }

    return Value(std::move(output));
}

void ExpressionMap::addDependencies(DepsTracker* deps) const {
    _input->addDependencies(deps);
    _each->addDependencies(deps);
}

void ExpressionMap::doInjectExpressionContext() {
    _input->injectExpressionContext(getExpressionContext());
    _each->injectExpressionContext(getExpressionContext());
}

/* ------------------------- ExpressionMeta ----------------------------- */

REGISTER_EXPRESSION(meta, ExpressionMeta::parse);
intrusive_ptr<Expression> ExpressionMeta::parse(BSONElement expr,
                                                const VariablesParseState& vpsIn) {
    uassert(17307, "$meta only supports string arguments", expr.type() == String);
    if (expr.valueStringData() == "textScore") {
        return new ExpressionMeta(MetaType::TEXT_SCORE);
    } else if (expr.valueStringData() == "randVal") {
        return new ExpressionMeta(MetaType::RAND_VAL);
    } else {
        uasserted(17308, "Unsupported argument to $meta: " + expr.String());
    }
}

ExpressionMeta::ExpressionMeta(MetaType metaType) : _metaType(metaType) {}

Value ExpressionMeta::serialize(bool explain) const {
    switch (_metaType) {
        case MetaType::TEXT_SCORE:
            return Value(DOC("$meta"
                             << "textScore"));
        case MetaType::RAND_VAL:
            return Value(DOC("$meta"
                             << "randVal"));
    }
    MONGO_UNREACHABLE;
}

Value ExpressionMeta::evaluateInternal(Variables* vars) const {
    const Document& root = vars->getRoot();
    switch (_metaType) {
        case MetaType::TEXT_SCORE:
            return root.hasTextScore() ? Value(root.getTextScore()) : Value();
        case MetaType::RAND_VAL:
            return root.hasRandMetaField() ? Value(root.getRandMetaField()) : Value();
    }
    MONGO_UNREACHABLE;
}

void ExpressionMeta::addDependencies(DepsTracker* deps) const {
    if (_metaType == MetaType::TEXT_SCORE) {
        deps->setNeedTextScore(true);
    }
}

/* ------------------------- ExpressionMillisecond ----------------------------- */

Value ExpressionMillisecond::evaluateInternal(Variables* vars) const {
    Value date(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(date.coerceToDate()));
}

int ExpressionMillisecond::extract(const long long date) {
    const int ms = date % 1000LL;
    // adding 1000 since dates before 1970 would have negative ms
    return ms >= 0 ? ms : 1000 + ms;
}

REGISTER_EXPRESSION(millisecond, ExpressionMillisecond::parse);
const char* ExpressionMillisecond::getOpName() const {
    return "$millisecond";
}

/* ------------------------- ExpressionMinute -------------------------- */

Value ExpressionMinute::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(minute, ExpressionMinute::parse);
const char* ExpressionMinute::getOpName() const {
    return "$minute";
}

/* ----------------------- ExpressionMod ---------------------------- */

Value ExpressionMod::evaluateInternal(Variables* vars) const {
    Value lhs = vpOperand[0]->evaluateInternal(vars);
    Value rhs = vpOperand[1]->evaluateInternal(vars);

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
        } else if (leftType == NumberLong || rightType == NumberLong) {
            // if either is long, return long
            long long left = lhs.coerceToLong();
            long long rightLong = rhs.coerceToLong();
            return Value(left % rightLong);
        }

        // lastly they must both be ints, return int
        int left = lhs.coerceToInt();
        int rightInt = rhs.coerceToInt();
        return Value(left % rightInt);
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

/* ------------------------ ExpressionMonth ----------------------------- */

Value ExpressionMonth::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(month, ExpressionMonth::parse);
const char* ExpressionMonth::getOpName() const {
    return "$month";
}

/* ------------------------- ExpressionMultiply ----------------------------- */

Value ExpressionMultiply::evaluateInternal(Variables* vars) const {
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
        Value val = vpOperand[i]->evaluateInternal(vars);

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
                if (mongoSignedMultiplyOverflow64(longProduct, val.coerceToLong(), &longProduct)) {
                    // The 'longProduct' would have overflowed, so we're abandoning it.
                    productType = NumberDouble;
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

/* ------------------------- ExpressionHour ----------------------------- */

Value ExpressionHour::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(hour, ExpressionHour::parse);
const char* ExpressionHour::getOpName() const {
    return "$hour";
}

/* ----------------------- ExpressionIfNull ---------------------------- */

Value ExpressionIfNull::evaluateInternal(Variables* vars) const {
    Value pLeft(vpOperand[0]->evaluateInternal(vars));
    if (!pLeft.nullish())
        return pLeft;

    Value pRight(vpOperand[1]->evaluateInternal(vars));
    return pRight;
}

REGISTER_EXPRESSION(ifNull, ExpressionIfNull::parse);
const char* ExpressionIfNull::getOpName() const {
    return "$ifNull";
}

/* ----------------------- ExpressionIn ---------------------------- */

Value ExpressionIn::evaluateInternal(Variables* vars) const {
    Value argument(vpOperand[0]->evaluateInternal(vars));
    Value arrayOfValues(vpOperand[1]->evaluateInternal(vars));

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

Value ExpressionIndexOfArray::evaluateInternal(Variables* vars) const {
    Value arrayArg = vpOperand[0]->evaluateInternal(vars);

    if (arrayArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40090,
            str::stream() << "$indexOfArray requires an array as a first argument, found: "
                          << typeName(arrayArg.getType()),
            arrayArg.isArray());

    std::vector<Value> array = arrayArg.getArray();

    Value searchItem = vpOperand[1]->evaluateInternal(vars);

    size_t startIndex = 0;
    if (vpOperand.size() > 2) {
        Value startIndexArg = vpOperand[2]->evaluateInternal(vars);
        uassertIfNotIntegralAndNonNegative(startIndexArg, getOpName(), "starting index");
        startIndex = static_cast<size_t>(startIndexArg.coerceToInt());
    }

    size_t endIndex = array.size();
    if (vpOperand.size() > 3) {
        Value endIndexArg = vpOperand[3]->evaluateInternal(vars);
        uassertIfNotIntegralAndNonNegative(endIndexArg, getOpName(), "ending index");
        // Don't let 'endIndex' exceed the length of the array.
        endIndex = std::min(array.size(), static_cast<size_t>(endIndexArg.coerceToInt()));
    }

    for (size_t i = startIndex; i < endIndex; i++) {
        if (getExpressionContext()->getValueComparator().evaluate(array[i] == searchItem)) {
            return Value(static_cast<int>(i));
        }
    }

    return Value(-1);
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

Value ExpressionIndexOfBytes::evaluateInternal(Variables* vars) const {
    Value stringArg = vpOperand[0]->evaluateInternal(vars);

    if (stringArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40091,
            str::stream() << "$indexOfBytes requires a string as the first argument, found: "
                          << typeName(stringArg.getType()),
            stringArg.getType() == String);
    const std::string& input = stringArg.getString();

    Value tokenArg = vpOperand[1]->evaluateInternal(vars);
    uassert(40092,
            str::stream() << "$indexOfBytes requires a string as the second argument, found: "
                          << typeName(tokenArg.getType()),
            tokenArg.getType() == String);
    const std::string& token = tokenArg.getString();

    size_t startIndex = 0;
    if (vpOperand.size() > 2) {
        Value startIndexArg = vpOperand[2]->evaluateInternal(vars);
        uassertIfNotIntegralAndNonNegative(startIndexArg, getOpName(), "starting index");
        startIndex = static_cast<size_t>(startIndexArg.coerceToInt());
    }

    size_t endIndex = input.size();
    if (vpOperand.size() > 3) {
        Value endIndexArg = vpOperand[3]->evaluateInternal(vars);
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

Value ExpressionIndexOfCP::evaluateInternal(Variables* vars) const {
    Value stringArg = vpOperand[0]->evaluateInternal(vars);

    if (stringArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40093,
            str::stream() << "$indexOfCP requires a string as the first argument, found: "
                          << typeName(stringArg.getType()),
            stringArg.getType() == String);
    const std::string& input = stringArg.getString();

    Value tokenArg = vpOperand[1]->evaluateInternal(vars);
    uassert(40094,
            str::stream() << "$indexOfCP requires a string as the second argument, found: "
                          << typeName(tokenArg.getType()),
            tokenArg.getType() == String);
    const std::string& token = tokenArg.getString();

    size_t startCodePointIndex = 0;
    if (vpOperand.size() > 2) {
        Value startIndexArg = vpOperand[2]->evaluateInternal(vars);
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

        uassert(
            40095, "$indexOfCP found bad UTF-8 in the input", !isContinuationByte(input[byteIx]));
        byteIx += getCodePointLength(input[byteIx]);
    }

    size_t endCodePointIndex = codePointLength;
    if (vpOperand.size() > 3) {
        Value endIndexArg = vpOperand[3]->evaluateInternal(vars);
        uassertIfNotIntegralAndNonNegative(endIndexArg, getOpName(), "ending index");

        // Don't let 'endCodePointIndex' exceed the number of code points in the string.
        endCodePointIndex =
            std::min(codePointLength, static_cast<size_t>(endIndexArg.coerceToInt()));
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

Value ExpressionLog::evaluateInternal(Variables* vars) const {
    Value argVal = vpOperand[0]->evaluateInternal(vars);
    Value baseVal = vpOperand[1]->evaluateInternal(vars);
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
        Variables emptyVars;
        return intrusive_ptr<Expression>(
            ExpressionConstant::create(getExpressionContext(), evaluateInternal(&emptyVars)));
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
                    Variables emptyVars;
                    optimizedOperands.emplace_back(ExpressionConstant::create(
                        getExpressionContext(), evaluateInternal(&emptyVars)));
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
            Variables emptyVars;
            optimizedOperands.emplace_back(
                ExpressionConstant::create(getExpressionContext(), evaluateInternal(&emptyVars)));
        } else {
            optimizedOperands.insert(
                optimizedOperands.end(), constExpressions.begin(), constExpressions.end());
        }

        vpOperand = std::move(optimizedOperands);
    }
    return this;
}

void ExpressionNary::addDependencies(DepsTracker* deps) const {
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

void ExpressionNary::doInjectExpressionContext() {
    for (auto&& operand : vpOperand) {
        operand->injectExpressionContext(getExpressionContext());
    }
}

/* ------------------------- ExpressionNot ----------------------------- */

Value ExpressionNot::evaluateInternal(Variables* vars) const {
    Value pOp(vpOperand[0]->evaluateInternal(vars));

    bool b = pOp.coerceToBool();
    return Value(!b);
}

REGISTER_EXPRESSION(not, ExpressionNot::parse);
const char* ExpressionNot::getOpName() const {
    return "$not";
}

/* -------------------------- ExpressionOr ----------------------------- */

Value ExpressionOr::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(vpOperand[i]->evaluateInternal(vars));
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

/* ----------------------- ExpressionPow ---------------------------- */

Value ExpressionPow::evaluateInternal(Variables* vars) const {
    Value baseVal = vpOperand[0]->evaluateInternal(vars);
    Value expVal = vpOperand[1]->evaluateInternal(vars);
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

    // base and exp are both integers.

    auto representableAsLong = [](long long base, long long exp) {
        // If exp is greater than 63 and base is not -1, 0, or 1, the result will overflow.
        // If exp is negative and the base is not -1 or 1, the result will be fractional.
        if (exp < 0 || exp > 63) {
            return std::abs(base) == 1 || base == 0;
        }

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

    long long baseLong = baseVal.getLong();
    long long expLong = expVal.getLong();

    // If the result cannot be represented as a long, return a double. Otherwise if either number
    // is a long, return a long. If both numbers are ints, then return an int if the result fits or
    // a long if it is too big.
    if (!representableAsLong(baseLong, expLong)) {
        return Value(std::pow(baseLong, expLong));
    }

    long long result = 1;
    // Use repeated multiplication, since pow() casts args to doubles which could result in loss of
    // precision if arguments are very large.
    for (int i = 0; i < expLong; i++) {
        result *= baseLong;
    }

    if (baseType == NumberLong || expType == NumberLong) {
        return Value(result);
    }
    return Value::createIntOrLong(result);
}

REGISTER_EXPRESSION(pow, ExpressionPow::parse);
const char* ExpressionPow::getOpName() const {
    return "$pow";
}

/* ------------------------- ExpressionRange ------------------------------ */

Value ExpressionRange::evaluateInternal(Variables* vars) const {
    Value startVal(vpOperand[0]->evaluateInternal(vars));
    Value endVal(vpOperand[1]->evaluateInternal(vars));

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

    int current = startVal.coerceToInt();
    int end = endVal.coerceToInt();

    int step = 1;
    if (vpOperand.size() == 3) {
        // A step was specified by the user.
        Value stepVal(vpOperand[2]->evaluateInternal(vars));

        uassert(34447,
                str::stream() << "$range requires a numeric starting value, found value of type:"
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

    std::vector<Value> output;

    while ((step > 0 ? current < end : current > end)) {
        output.push_back(Value(current));
        current += step;
    }

    return Value(output);
}

REGISTER_EXPRESSION(range, ExpressionRange::parse);
const char* ExpressionRange::getOpName() const {
    return "$range";
}

/* ------------------------ ExpressionReduce ------------------------------ */

REGISTER_EXPRESSION(reduce, ExpressionReduce::parse);
intrusive_ptr<Expression> ExpressionReduce::parse(BSONElement expr,
                                                  const VariablesParseState& vps) {
    uassert(40075,
            str::stream() << "$reduce requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<ExpressionReduce> reduce(new ExpressionReduce());

    // vpsSub is used only to parse 'in', which must have access to $$this and $$value.
    VariablesParseState vpsSub(vps);
    reduce->_thisVar = vpsSub.defineVariable("this");
    reduce->_valueVar = vpsSub.defineVariable("value");

    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "input") {
            reduce->_input = parseOperand(elem, vps);
        } else if (field == "initialValue") {
            reduce->_initial = parseOperand(elem, vps);
        } else if (field == "in") {
            reduce->_in = parseOperand(elem, vpsSub);
        } else {
            uasserted(40076, str::stream() << "$reduce found an unknown argument: " << field);
        }
    }

    uassert(40077, "$reduce requires 'input' to be specified", reduce->_input);
    uassert(40078, "$reduce requires 'initialValue' to be specified", reduce->_initial);
    uassert(40079, "$reduce requires 'in' to be specified", reduce->_in);

    return reduce;
}

Value ExpressionReduce::evaluateInternal(Variables* vars) const {
    Value inputVal = _input->evaluateInternal(vars);

    if (inputVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40080,
            str::stream() << "$reduce requires that 'input' be an array, found: "
                          << inputVal.toString(),
            inputVal.isArray());

    Value accumulatedValue = _initial->evaluateInternal(vars);

    for (auto&& elem : inputVal.getArray()) {
        vars->setValue(_thisVar, elem);
        vars->setValue(_valueVar, accumulatedValue);

        accumulatedValue = _in->evaluateInternal(vars);
    }

    return accumulatedValue;
}

intrusive_ptr<Expression> ExpressionReduce::optimize() {
    _input = _input->optimize();
    _initial = _initial->optimize();
    _in = _in->optimize();
    return this;
}

void ExpressionReduce::addDependencies(DepsTracker* deps) const {
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

void ExpressionReduce::doInjectExpressionContext() {
    _input->injectExpressionContext(getExpressionContext());
    _initial->injectExpressionContext(getExpressionContext());
    _in->injectExpressionContext(getExpressionContext());
}

/* ------------------------ ExpressionReverseArray ------------------------ */

Value ExpressionReverseArray::evaluateInternal(Variables* vars) const {
    Value input(vpOperand[0]->evaluateInternal(vars));

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

/* ------------------------- ExpressionSecond ----------------------------- */

Value ExpressionSecond::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(second, ExpressionSecond::parse);
const char* ExpressionSecond::getOpName() const {
    return "$second";
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

Value ExpressionSetDifference::evaluateInternal(Variables* vars) const {
    const Value lhs = vpOperand[0]->evaluateInternal(vars);
    const Value rhs = vpOperand[1]->evaluateInternal(vars);

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

Value ExpressionSetEquals::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();
    const auto& valueComparator = getExpressionContext()->getValueComparator();
    ValueSet lhs = valueComparator.makeOrderedValueSet();

    for (size_t i = 0; i < n; i++) {
        const Value nextEntry = vpOperand[i]->evaluateInternal(vars);
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

Value ExpressionSetIntersection::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();
    const auto& valueComparator = getExpressionContext()->getValueComparator();
    ValueSet currentIntersection = valueComparator.makeOrderedValueSet();
    for (size_t i = 0; i < n; i++) {
        const Value nextEntry = vpOperand[i]->evaluateInternal(vars);
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

Value ExpressionSetIsSubset::evaluateInternal(Variables* vars) const {
    const Value lhs = vpOperand[0]->evaluateInternal(vars);
    const Value rhs = vpOperand[1]->evaluateInternal(vars);

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
    Optimized(const ValueSet& cachedRhsSet, const ExpressionVector& operands)
        : _cachedRhsSet(cachedRhsSet) {
        vpOperand = operands;
    }

    virtual Value evaluateInternal(Variables* vars) const {
        const Value lhs = vpOperand[0]->evaluateInternal(vars);

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

        intrusive_ptr<Expression> optimizedWithConstant(new Optimized(
            arrayToSet(rhs, getExpressionContext()->getValueComparator()), vpOperand));
        optimizedWithConstant->injectExpressionContext(getExpressionContext());
        return optimizedWithConstant;
    }
    return optimized;
}

REGISTER_EXPRESSION(setIsSubset, ExpressionSetIsSubset::parse);
const char* ExpressionSetIsSubset::getOpName() const {
    return "$setIsSubset";
}

/* ----------------------- ExpressionSetUnion ---------------------------- */

Value ExpressionSetUnion::evaluateInternal(Variables* vars) const {
    ValueSet unionedSet = getExpressionContext()->getValueComparator().makeOrderedValueSet();
    const size_t n = vpOperand.size();
    for (size_t i = 0; i < n; i++) {
        const Value newEntries = vpOperand[i]->evaluateInternal(vars);
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

Value ExpressionIsArray::evaluateInternal(Variables* vars) const {
    Value argument = vpOperand[0]->evaluateInternal(vars);
    return Value(argument.isArray());
}

REGISTER_EXPRESSION(isArray, ExpressionIsArray::parse);
const char* ExpressionIsArray::getOpName() const {
    return "$isArray";
}

/* ----------------------- ExpressionSlice ---------------------------- */

Value ExpressionSlice::evaluateInternal(Variables* vars) const {
    const size_t n = vpOperand.size();

    Value arrayVal = vpOperand[0]->evaluateInternal(vars);
    // Could be either a start index or the length from 0.
    Value arg2 = vpOperand[1]->evaluateInternal(vars);

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

        Value countVal = vpOperand[2]->evaluateInternal(vars);

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

Value ExpressionSize::evaluateInternal(Variables* vars) const {
    Value array = vpOperand[0]->evaluateInternal(vars);

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

Value ExpressionSplit::evaluateInternal(Variables* vars) const {
    Value inputArg = vpOperand[0]->evaluateInternal(vars);
    Value separatorArg = vpOperand[1]->evaluateInternal(vars);

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

Value ExpressionStrcasecmp::evaluateInternal(Variables* vars) const {
    Value pString1(vpOperand[0]->evaluateInternal(vars));
    Value pString2(vpOperand[1]->evaluateInternal(vars));

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

Value ExpressionSubstrBytes::evaluateInternal(Variables* vars) const {
    Value pString(vpOperand[0]->evaluateInternal(vars));
    Value pLower(vpOperand[1]->evaluateInternal(vars));
    Value pLength(vpOperand[2]->evaluateInternal(vars));

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

    string::size_type lower = static_cast<string::size_type>(pLower.coerceToLong());
    string::size_type length = static_cast<string::size_type>(pLength.coerceToLong());

    uassert(28656,
            str::stream() << getOpName()
                          << ":  Invalid range, starting index is a UTF-8 continuation byte.",
            (lower >= str.length() || !isContinuationByte(str[lower])));

    // Check the byte after the last character we'd return. If it is a continuation byte, that
    // means we're in the middle of a UTF-8 character.
    uassert(
        28657,
        str::stream() << getOpName()
                      << ":  Invalid range, ending index is in the middle of a UTF-8 character.",
        (lower + length >= str.length() || !isContinuationByte(str[lower + length])));

    if (lower >= str.length()) {
        // If lower > str.length() then string::substr() will throw out_of_range, so return an
        // empty string if lower is not a valid string index.
        return Value("");
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

Value ExpressionSubstrCP::evaluateInternal(Variables* vars) const {
    Value inputVal(vpOperand[0]->evaluateInternal(vars));
    Value lowerVal(vpOperand[1]->evaluateInternal(vars));
    Value lengthVal(vpOperand[2]->evaluateInternal(vars));

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
            return Value("");
        }
        uassert(34456,
                str::stream() << getOpName() << ": invalid UTF-8 string",
                !isContinuationByte(str[startIndexBytes]));
        size_t codePointLength = getCodePointLength(str[startIndexBytes]);
        uassert(
            34457, str::stream() << getOpName() << ": invalid UTF-8 string", codePointLength <= 4);
        startIndexBytes += codePointLength;
    }

    size_t endIndexBytes = startIndexBytes;

    for (int i = 0; i < length && endIndexBytes < str.size(); i++) {
        uassert(34458,
                str::stream() << getOpName() << ": invalid UTF-8 string",
                !isContinuationByte(str[endIndexBytes]));
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

Value ExpressionStrLenBytes::evaluateInternal(Variables* vars) const {
    Value str(vpOperand[0]->evaluateInternal(vars));

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

Value ExpressionStrLenCP::evaluateInternal(Variables* vars) const {
    Value val(vpOperand[0]->evaluateInternal(vars));

    uassert(34471,
            str::stream() << "$strLenCP requires a string argument, found: "
                          << typeName(val.getType()),
            val.getType() == String);

    std::string stringVal = val.getString();

    size_t strLen = 0;
    for (char byte : stringVal) {
        strLen += !isContinuationByte(byte);
    }

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

Value ExpressionSubtract::evaluateInternal(Variables* vars) const {
    Value lhs = vpOperand[0]->evaluateInternal(vars);
    Value rhs = vpOperand[1]->evaluateInternal(vars);

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
        long long right = rhs.coerceToLong();
        long long left = lhs.coerceToLong();
        return Value(left - right);
    } else if (diffType == NumberInt) {
        long long right = rhs.coerceToLong();
        long long left = lhs.coerceToLong();
        return Value::createIntOrLong(left - right);
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else if (lhs.getType() == Date) {
        if (rhs.getType() == Date) {
            long long timeDelta = lhs.getDate() - rhs.getDate();
            return Value(timeDelta);
        } else if (rhs.numeric()) {
            long long millisSinceEpoch = lhs.getDate() - rhs.coerceToLong();
            return Value(Date_t::fromMillisSinceEpoch(millisSinceEpoch));
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
const char* ExpressionSwitch::getOpName() const {
    return "$switch";
}

Value ExpressionSwitch::evaluateInternal(Variables* vars) const {
    for (auto&& branch : _branches) {
        Value caseExpression(branch.first->evaluateInternal(vars));

        if (caseExpression.coerceToBool()) {
            return branch.second->evaluateInternal(vars);
        }
    }

    uassert(40066,
            "$switch could not find a matching branch for an input, and no default was specified.",
            _default);

    return _default->evaluateInternal(vars);
}

boost::intrusive_ptr<Expression> ExpressionSwitch::parse(BSONElement expr,
                                                         const VariablesParseState& vps) {
    uassert(40060,
            str::stream() << "$switch requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<ExpressionSwitch> expression(new ExpressionSwitch());

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
                        branchExpression.first = parseOperand(branchElement, vps);
                    } else if (branchField == "then") {
                        branchExpression.second = parseOperand(branchElement, vps);
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
            expression->_default = parseOperand(elem, vps);
        } else {
            uasserted(40067, str::stream() << "$switch found an unknown argument: " << field);
        }
    }

    uassert(40068, "$switch requires at least one branch.", !expression->_branches.empty());

    return expression;
}

void ExpressionSwitch::addDependencies(DepsTracker* deps) const {
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

void ExpressionSwitch::doInjectExpressionContext() {
    if (_default) {
        _default->injectExpressionContext(getExpressionContext());
    }

    for (auto&& pair : _branches) {
        pair.first->injectExpressionContext(getExpressionContext());
        pair.second->injectExpressionContext(getExpressionContext());
    }
}

/* ------------------------- ExpressionToLower ----------------------------- */

Value ExpressionToLower::evaluateInternal(Variables* vars) const {
    Value pString(vpOperand[0]->evaluateInternal(vars));
    string str = pString.coerceToString();
    boost::to_lower(str);
    return Value(str);
}

REGISTER_EXPRESSION(toLower, ExpressionToLower::parse);
const char* ExpressionToLower::getOpName() const {
    return "$toLower";
}

/* ------------------------- ExpressionToUpper -------------------------- */

Value ExpressionToUpper::evaluateInternal(Variables* vars) const {
    Value pString(vpOperand[0]->evaluateInternal(vars));
    string str(pString.coerceToString());
    boost::to_upper(str);
    return Value(str);
}

REGISTER_EXPRESSION(toUpper, ExpressionToUpper::parse);
const char* ExpressionToUpper::getOpName() const {
    return "$toUpper";
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

Value ExpressionType::evaluateInternal(Variables* vars) const {
    Value val(vpOperand[0]->evaluateInternal(vars));
    return Value(typeName(val.getType()));
}

REGISTER_EXPRESSION(type, ExpressionType::parse);
const char* ExpressionType::getOpName() const {
    return "$type";
}

/* ------------------------- ExpressionWeek ----------------------------- */

Value ExpressionWeek::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

int ExpressionWeek::extract(const tm& tm) {
    int dayOfWeek = tm.tm_wday;
    int dayOfYear = tm.tm_yday;
    int prevSundayDayOfYear = dayOfYear - dayOfWeek;    // may be negative
    int nextSundayDayOfYear = prevSundayDayOfYear + 7;  // must be positive

    // Return the zero based index of the week of the next sunday, equal to the one based index
    // of the week of the previous sunday, which is to be returned.
    int nextSundayWeek = nextSundayDayOfYear / 7;

    // Verify that the week calculation is consistent with strftime "%U".
    DEV {
        char buf[3];
        verify(strftime(buf, 3, "%U", &tm));
        verify(int(str::toUnsigned(buf)) == nextSundayWeek);
    }

    return nextSundayWeek;
}

REGISTER_EXPRESSION(week, ExpressionWeek::parse);
const char* ExpressionWeek::getOpName() const {
    return "$week";
}

/* ------------------------- ExpressionIsoDayOfWeek --------------------- */

Value ExpressionIsoDayOfWeek::evaluateInternal(Variables* vars) const {
    Value date(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(date.coerceToTm()));
}

int ExpressionIsoDayOfWeek::extract(const tm& tm) {
    // translate from sunday=0 … saturday=6 to monday=1 … sunday=7
    return (tm.tm_wday - 7) % 7 + 7;
}

REGISTER_EXPRESSION(isoDayOfWeek, ExpressionIsoDayOfWeek::parse);
const char* ExpressionIsoDayOfWeek::getOpName() const {
    return "$isoDayOfWeek";
}

/* ------------------------- ExpressionIsoWeekYear ---------------------- */

Value ExpressionIsoWeekYear::evaluateInternal(Variables* vars) const {
    Value date(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(date.coerceToTm()));
}

int ExpressionIsoWeekYear::extract(const tm& tm) {
    if (tm.tm_mon > 0 && tm.tm_mon < 11) {
        // If month is between February and November, it is just the year given.
        return tm.tm_year + 1900;
    } else if (tm.tm_mon == 0) {
        // In January we need to check if the week belongs to previous year.
        int isoWeek = ExpressionIsoWeek::extract(tm);
        if (isoWeek > 51) {  // Weeks 52 and 53 belong to the previous year.
            return tm.tm_year + 1900 - 1;
        } else {  // All other weeks belong to given year.
            return tm.tm_year + 1900;
        }
    } else {
        // A week 1 in December belongs to the next year.
        int isoWeek = ExpressionIsoWeek::extract(tm);
        if (isoWeek == 1) {
            return tm.tm_year + 1900 + 1;
        } else {
            return tm.tm_year + 1900;
        }
    }
}

REGISTER_EXPRESSION(isoWeekYear, ExpressionIsoWeekYear::parse);
const char* ExpressionIsoWeekYear::getOpName() const {
    return "$isoWeekYear";
}

/* ------------------------- ExpressionIsoWeek -------------------------- */

namespace {
bool isLeapYear(int year) {
    if (year % 4 != 0) {
        // Not a leap year because a leap year must be divisable by 4.
        return false;
    } else if (year % 100 != 0) {
        // Every year that is divisable by 100 is a leap year.
        return true;
    } else if (year % 400 != 0) {
        // Every 400th year is not a leap year, althoght it is divisable by 4.
        return false;
    } else {
        // Every year divisable by 4 but not 400 is a leap year.
        return true;
    }
}

int lastWeek(int year) {
    // Create YYYY-12-31T23:59:59 so only 1 second left to new year.
    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = 11;
    tm.tm_mday = 31;
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 59;
    mktime(&tm);

    // From: https://en.wikipedia.org/wiki/ISO_week_date#Last_week :
    // If 31 December is on a Monday, Tuesday or Wednesday, it is in week 01 of the next year. If
    // it is on a Thursday, it is in week 53 of the year just ending; if on a Friday it is in week
    // 52 (or 53 if the year just ending is a leap year); if on a Saturday or Sunday, it is in week
    // 52 of the year just ending.
    if (tm.tm_wday > 0 && tm.tm_wday < 4) {  // Mon(1), Tue(2), and Wed(3)
        return 1;
    } else if (tm.tm_wday == 4) {  // Thu (4)
        return 53;
    } else if (tm.tm_wday == 5) {  // Fri (5)
        // On Fri it's week 52 for non leap years and 53 for leap years.
        // https://en.wikipedia.org/wiki/Leap_year#Algorithm
        if (isLeapYear(year)) {
            return 53;
        } else {
            return 52;
        }
    } else {  // Sat (6) or Sun (0)
        return 52;
    }
}
}

Value ExpressionIsoWeek::evaluateInternal(Variables* vars) const {
    Value date(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(date.coerceToTm()));
}

// Quote https://en.wikipedia.org/wiki/ISO_week_date :
// Weeks start with Monday. The first week of a year is the week that contains the first Thursday of
// the year (and, hence, always contains 4 January).
int ExpressionIsoWeek::extract(const tm& tm) {
    // Calculation taken from:
    // https://en.wikipedia.org/wiki/ISO_week_date#Calculating_the_week_number_of_a_given_date
    //
    // week(date) = floor( (ordinal(data) - weekday(date) + 10) / 7 )
    //
    // The first week must contain the first Thursday, therefore the `+ 10` after subtracting the
    // weekday. Example: 2016-01-07 is the first Thursday
    // ordinal(2016-01-07) => 7
    // weekday(2016-01-07) => 4
    //
    // floor((7-4+10)/7) = floor(13/7) => 1
    //
    // week(date)    = isoWeek
    // ordinal(date) = isoDayOfYear
    // weekday(date) = isoDayOfWeek
    int isoDayOfWeek = ExpressionIsoDayOfWeek::extract(tm);
    int isoDayOfYear = tm.tm_yday + 1;
    int isoWeek = (isoDayOfYear - isoDayOfWeek + 10) / 7;

    // There is no week 0, so it must be the last week of the previous year.
    if (isoWeek < 1) {
        return lastWeek(tm.tm_year + 1900 - 1);
        // If the calculated week is 53 and bigger than the last week, than it is the first week of
        // the
        // next year.
    } else if (isoWeek == 53 && isoWeek > lastWeek(tm.tm_year + 1900)) {
        return 1;
        // It is just the week calculated
    } else {
        return isoWeek;
    }
}

REGISTER_EXPRESSION(isoWeek, ExpressionIsoWeek::parse);
const char* ExpressionIsoWeek::getOpName() const {
    return "$isoWeek";
}

/* ------------------------- ExpressionYear ----------------------------- */

Value ExpressionYear::evaluateInternal(Variables* vars) const {
    Value pDate(vpOperand[0]->evaluateInternal(vars));
    return Value(extract(pDate.coerceToTm()));
}

REGISTER_EXPRESSION(year, ExpressionYear::parse);
const char* ExpressionYear::getOpName() const {
    return "$year";
}

/* -------------------------- ExpressionZip ------------------------------ */

REGISTER_EXPRESSION(zip, ExpressionZip::parse);
intrusive_ptr<Expression> ExpressionZip::parse(BSONElement expr, const VariablesParseState& vps) {
    uassert(34460,
            str::stream() << "$zip only supports an object as an argument, found "
                          << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<ExpressionZip> newZip(new ExpressionZip());

    for (auto&& elem : expr.Obj()) {
        const auto field = elem.fieldNameStringData();
        if (field == "inputs") {
            uassert(34461,
                    str::stream() << "inputs must be an array of expressions, found "
                                  << typeName(elem.type()),
                    elem.type() == Array);
            for (auto&& subExpr : elem.Array()) {
                newZip->_inputs.push_back(parseOperand(subExpr, vps));
            }
        } else if (field == "defaults") {
            uassert(34462,
                    str::stream() << "defaults must be an array of expressions, found "
                                  << typeName(elem.type()),
                    elem.type() == Array);
            for (auto&& subExpr : elem.Array()) {
                newZip->_defaults.push_back(parseOperand(subExpr, vps));
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

Value ExpressionZip::evaluateInternal(Variables* vars) const {
    // Evaluate input values.
    vector<vector<Value>> inputValues;
    inputValues.reserve(_inputs.size());

    size_t minArraySize = 0;
    size_t maxArraySize = 0;
    for (size_t i = 0; i < _inputs.size(); i++) {
        Value evalExpr = _inputs[i]->evaluateInternal(vars);
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
            evaluatedDefaults[i] = _defaults[i]->evaluateInternal(vars);
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

void ExpressionZip::addDependencies(DepsTracker* deps) const {
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

void ExpressionZip::doInjectExpressionContext() {
    for (auto&& expr : _inputs) {
        expr->injectExpressionContext(getExpressionContext());
    }

    for (auto&& expr : _defaults) {
        expr->injectExpressionContext(getExpressionContext());
    }
}

const char* ExpressionZip::getOpName() const {
    return "$zip";
}
}
