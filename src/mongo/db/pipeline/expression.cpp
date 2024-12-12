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

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <array>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>
// IWYU pragma: no_include <pstl/glue_algorithm_defs.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"

#include "mongo/base/parse_number.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/hasher.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_parser_gen.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/bson/dotted_path_support.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/record_id.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/text.h"

namespace mongo {
using Parser = Expression::Parser;

using boost::intrusive_ptr;
using std::move;
using std::pair;
using std::string;
using std::vector;

Value ExpressionConstant::serializeConstant(const SerializationOptions& opts,
                                            Value val,
                                            bool wrapRepresentativeValue) {
    if (val.missing()) {
        return Value("$$REMOVE"_sd);
    }
    // It's safer to wrap constants in $const when generating representative shapes to avoid
    // ambiguity when re-parsing (SERVER-88296, SERVER-85376). However, we allow certain expressions
    // to override this behavior in order to reduce shape verbosity if the expression takes many
    // constant arguments (e.g. variadic expressions - SERVER-84159).
    // Debug shapes never wrap constants in $const to reduce shape size (and because re-parsing
    // support is not a consideration there).
    if ((opts.literalPolicy == LiteralSerializationPolicy::kUnchanged) ||
        (wrapRepresentativeValue &&
         opts.literalPolicy == LiteralSerializationPolicy::kToRepresentativeParseableValue)) {
        return Value(DOC("$const" << opts.serializeLiteral(val)));
    }

    return opts.serializeLiteral(val);
}

/* --------------------------- Expression ------------------------------ */

string Expression::removeFieldPrefix(const string& prefixedField) {
    uassert(16419,
            str::stream() << "field path must not contain embedded null characters",
            prefixedField.find('\0') == string::npos);

    const char* pPrefixedField = prefixedField.c_str();
    uassert(15982,
            str::stream() << "field path references must be prefixed with a '$' ('" << prefixedField
                          << "'",
            pPrefixedField[0] == '$');

    return string(pPrefixedField + 1);
}

intrusive_ptr<Expression> Expression::parseObject(ExpressionContext* const expCtx,
                                                  BSONObj obj,
                                                  const VariablesParseState& vps) {
    if (obj.isEmpty()) {
        return ExpressionConstant::create(expCtx, Value(Document{}));
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
    AllowedWithApiStrict allowedWithApiStrict;
    AllowedWithClientType allowedWithClientType;
    boost::optional<FeatureFlag> featureFlag;
};

StringMap<ParserRegistration> parserMap;
}  // namespace

void Expression::registerExpression(string key,
                                    Parser parser,
                                    AllowedWithApiStrict allowedWithApiStrict,
                                    AllowedWithClientType allowedWithClientType,
                                    boost::optional<FeatureFlag> featureFlag) {
    auto op = parserMap.find(key);
    massert(17064,
            str::stream() << "Duplicate expression (" << key << ") registered.",
            op == parserMap.end());
    parserMap[key] =
        ParserRegistration{parser, allowedWithApiStrict, allowedWithClientType, featureFlag};
    // Add this expression to the global map of operator counters for expressions.
    operatorCountersAggExpressions.addCounter(key);
}

intrusive_ptr<Expression> Expression::parseExpression(ExpressionContext* const expCtx,
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

    auto& entry = it->second;
    expCtx->throwIfFeatureFlagIsNotEnabledOnFCV(opName, entry.featureFlag);

    if (expCtx->getOperationContext()) {
        assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                       opName,
                                       entry.allowedWithApiStrict,
                                       entry.allowedWithClientType);
    }

    // Increment the counter for this expression in the current context.
    expCtx->incrementAggExprCounter(opName);
    return entry.parser(expCtx, obj.firstElement(), vps);
}

Expression::ExpressionVector ExpressionNary::parseArguments(ExpressionContext* const expCtx,
                                                            BSONElement exprElement,
                                                            const VariablesParseState& vps) {
    ExpressionVector out;
    if (exprElement.type() == Array) {
        for (auto&& elem : exprElement.Obj()) {
            out.push_back(Expression::parseOperand(expCtx, elem, vps));
        }
    } else {  // Assume it's an operand that accepts a single argument.
        out.push_back(Expression::parseOperand(expCtx, exprElement, vps));
    }

    return out;
}

intrusive_ptr<Expression> Expression::parseOperand(ExpressionContext* const expCtx,
                                                   BSONElement exprElement,
                                                   const VariablesParseState& vps) {
    BSONType type = exprElement.type();

    if (type == String && exprElement.valueStringData().starts_with('$')) {
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

bool Expression::isExpressionName(StringData name) {
    return parserMap.find(name) != parserMap.end();
}

/* ------------------------- Register Date Expressions ----------------------------- */

namespace {

template <typename SubClass>
boost::intrusive_ptr<Expression> parseDateExpressionAcceptingTimeZone(
    ExpressionContext* const expCtx,
    BSONElement operatorElem,
    const VariablesParseState& variablesParseState) {
    if (operatorElem.type() == BSONType::Object) {
        if (operatorElem.embeddedObject().firstElementFieldName()[0] == '$') {
            // Assume this is an expression specification representing the date argument
            // like {$add: [<date>, 1000]}.
            return new SubClass(expCtx,
                                Expression::parseObject(
                                    expCtx, operatorElem.embeddedObject(), variablesParseState));
        } else {
            // It's an object specifying the date and timezone options like {date: <date>,
            // timezone: <timezone>}.
            auto opName = operatorElem.fieldNameStringData();
            boost::intrusive_ptr<Expression> date;
            boost::intrusive_ptr<Expression> timeZone;
            for (const auto& subElem : operatorElem.embeddedObject()) {
                auto argName = subElem.fieldNameStringData();
                if (argName == "date"_sd) {
                    date = Expression::parseOperand(expCtx, subElem, variablesParseState);
                } else if (argName == "timezone"_sd) {
                    timeZone = Expression::parseOperand(expCtx, subElem, variablesParseState);
                } else {
                    uasserted(40535,
                              str::stream() << "unrecognized option to " << opName << ": \""
                                            << argName << "\"");
                }
            }
            uassert(40539,
                    str::stream() << "missing 'date' argument to " << opName
                                  << ", provided: " << operatorElem,
                    date);
            return new SubClass(expCtx, std::move(date), std::move(timeZone));
        }
    } else if (operatorElem.type() == BSONType::Array) {
        auto elems = operatorElem.Array();
        uassert(40536,
                str::stream() << operatorElem.fieldNameStringData()
                              << " accepts exactly one argument if given an array, but was given "
                              << elems.size(),
                elems.size() == 1);
        // We accept an argument wrapped in a single array. For example, either {$week: <date>}
        // or {$week: [<date>]} are valid, but not {$week: [{date: <date>}]}.
        return new SubClass(expCtx,
                            Expression::parseOperand(expCtx, elems[0], variablesParseState));
    }
    // Exhausting the other possibilities, we are left with a literal value which should be
    // treated as the date argument.
    return new SubClass(expCtx,
                        Expression::parseOperand(expCtx, operatorElem, variablesParseState));
}

}  // namespace

REGISTER_STABLE_EXPRESSION(dayOfMonth, parseDateExpressionAcceptingTimeZone<ExpressionDayOfMonth>);
Value ExpressionDayOfMonth::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(dayOfWeek, parseDateExpressionAcceptingTimeZone<ExpressionDayOfWeek>);
Value ExpressionDayOfWeek::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(dayOfYear, parseDateExpressionAcceptingTimeZone<ExpressionDayOfYear>);
Value ExpressionDayOfYear::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(hour, parseDateExpressionAcceptingTimeZone<ExpressionHour>);
Value ExpressionHour::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(isoDayOfWeek,
                           parseDateExpressionAcceptingTimeZone<ExpressionIsoDayOfWeek>);
Value ExpressionIsoDayOfWeek::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(isoWeek, parseDateExpressionAcceptingTimeZone<ExpressionIsoWeek>);
Value ExpressionIsoWeek::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(isoWeekYear,
                           parseDateExpressionAcceptingTimeZone<ExpressionIsoWeekYear>);
Value ExpressionIsoWeekYear::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(millisecond,
                           parseDateExpressionAcceptingTimeZone<ExpressionMillisecond>);
Value ExpressionMillisecond::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(minute, parseDateExpressionAcceptingTimeZone<ExpressionMinute>);
Value ExpressionMinute::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(month, parseDateExpressionAcceptingTimeZone<ExpressionMonth>);
Value ExpressionMonth::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(second, parseDateExpressionAcceptingTimeZone<ExpressionSecond>);
Value ExpressionSecond::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(week, parseDateExpressionAcceptingTimeZone<ExpressionWeek>);
Value ExpressionWeek::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(year, parseDateExpressionAcceptingTimeZone<ExpressionYear>);
Value ExpressionYear::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> DateExpressionAcceptingTimeZone::optimize() {
    _children[_kDate] = _children[_kDate]->optimize();
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }
    if (ExpressionConstant::allNullOrConstant({_children[_kDate], _children[_kTimeZone]})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone =
            exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                           Document{},
                                           _children[_kTimeZone].get(),
                                           &(getExpressionContext()->variables));
    }
    return this;
}

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

REGISTER_STABLE_EXPRESSION(abs, ExpressionAbs::parse);
const char* ExpressionAbs::getOpName() const {
    return "$abs";
}

/* ------------------------- ExpressionAdd ----------------------------- */

namespace {

/**
 * We'll try to return the narrowest possible result value while avoiding overflow or implicit use
 * of decimal types. To do that, compute separate sums for long, double and decimal values, and
 * track the current widest type. The long sum will be converted to double when the first double
 * value is seen or when long arithmetic would overflow.
 */
class AddState {
public:
    /**
     * Update the internal state with another operand. It is up to the caller to validate that the
     * operand is of a proper type.
     */
    void operator+=(const Value& operand) {
        auto oldWidestType = widestType;
        // Dates are represented by the long number of milliseconds since the unix epoch, so we can
        // treat them as regular numeric values for the purposes of addition after making sure that
        // only one date is present in the operand list.
        Value valToAdd;
        if (operand.getType() == Date) {
            uassert(16612, "only one date allowed in an $add expression", !isDate);
            Value oldValue = getValue();
            longTotal = 0;
            addToDateValue(oldValue);
            isDate = true;
            valToAdd = Value(operand.getDate().toMillisSinceEpoch());
        } else {
            widestType = Value::getWidestNumeric(widestType, operand.getType());
            valToAdd = operand;
        }

        if (isDate) {
            addToDateValue(valToAdd);
            return;
        }

        // If this operation widens the return type, perform any necessary type conversions.
        if (oldWidestType != widestType) {
            switch (widestType) {
                case NumberLong:
                    // Int -> Long is handled by the same sum.
                    break;
                case NumberDouble:
                    // Int/Long -> Double converts the existing longTotal to a doubleTotal.
                    doubleTotal = longTotal;
                    break;
                case NumberDecimal:
                    // Convert the right total to NumberDecimal by looking at the old widest type.
                    switch (oldWidestType) {
                        case NumberInt:
                        case NumberLong:
                            decimalTotal = Decimal128(longTotal);
                            break;
                        case NumberDouble:
                            decimalTotal = Decimal128(doubleTotal);
                            break;
                        default:
                            MONGO_UNREACHABLE;
                    }
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        // Perform the add operation.
        switch (widestType) {
            case NumberInt:
            case NumberLong:
                // If the long long arithmetic overflows, promote the result to a NumberDouble and
                // start incrementing the doubleTotal.
                long long newLongTotal;
                if (overflow::add(longTotal, valToAdd.coerceToLong(), &newLongTotal)) {
                    widestType = NumberDouble;
                    doubleTotal = longTotal + valToAdd.coerceToDouble();
                } else {
                    longTotal = newLongTotal;
                }
                break;
            case NumberDouble:
                doubleTotal += valToAdd.coerceToDouble();
                break;
            case NumberDecimal:
                decimalTotal = decimalTotal.add(valToAdd.coerceToDecimal());
                break;
            default:
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream() << "$add only supports numeric or date types, not "
                                        << typeName(valToAdd.getType()));
        }
    }

    Value getValue() const {
        // If one of the operands was a date, then return long value as Date.
        if (isDate) {
            return Value(Date_t::fromMillisSinceEpoch(longTotal));
        } else {
            switch (widestType) {
                case NumberInt:
                    return Value::createIntOrLong(longTotal);
                case NumberLong:
                    return Value(longTotal);
                case NumberDouble:
                    return Value(doubleTotal);
                case NumberDecimal:
                    return Value(decimalTotal);
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

private:
    // Convert 'valToAdd' into the data type used for dates (long long) and add it to 'longTotal'.
    void addToDateValue(Value valToAdd) {
        switch (valToAdd.getType()) {
            case NumberInt:
            case NumberLong:
                if (overflow::add(longTotal, valToAdd.coerceToLong(), &longTotal)) {
                    uasserted(ErrorCodes::Overflow, "date overflow");
                }
                break;
            case NumberDouble: {
                using limits = std::numeric_limits<long long>;
                double doubleToAdd = valToAdd.coerceToDouble();
                uassert(ErrorCodes::Overflow,
                        "date overflow",
                        // The upper bound is exclusive because it rounds up when it is cast to
                        // a double.
                        doubleToAdd >= static_cast<double>(limits::min()) &&
                            doubleToAdd < static_cast<double>(limits::max()));

                if (overflow::add(longTotal, llround(doubleToAdd), &longTotal)) {
                    uasserted(ErrorCodes::Overflow, "date overflow");
                }
                break;
            }
            case NumberDecimal: {
                Decimal128 decimalToAdd = valToAdd.coerceToDecimal();

                std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
                std::int64_t longToAdd = decimalToAdd.toLong(&signalingFlags);
                if (signalingFlags != Decimal128::SignalingFlag::kNoFlag ||
                    overflow::add(longTotal, longToAdd, &longTotal)) {
                    uasserted(ErrorCodes::Overflow, "date overflow");
                }
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    long long longTotal = 0;
    double doubleTotal = 0;
    Decimal128 decimalTotal;
    BSONType widestType = NumberInt;
    bool isDate = false;
};

Status checkAddOperandType(Value val) {
    if (!val.numeric() && val.getType() != Date) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "$add only supports numeric or date types, not "
                                    << typeName(val.getType()));
    }

    return Status::OK();
}
}  // namespace

StatusWith<Value> ExpressionAdd::apply(Value lhs, Value rhs) {
    if (lhs.nullish())
        return Value(BSONNULL);
    if (Status s = checkAddOperandType(lhs); !s.isOK())
        return s;
    if (rhs.nullish())
        return Value(BSONNULL);
    if (Status s = checkAddOperandType(rhs); !s.isOK())
        return s;

    AddState state;
    state += lhs;
    state += rhs;
    return state.getValue();
}

Value ExpressionAdd::evaluate(const Document& root, Variables* variables) const {
    AddState state;
    for (auto&& child : _children) {
        Value val = child->evaluate(root, variables);
        if (val.nullish())
            return Value(BSONNULL);
        uassertStatusOK(checkAddOperandType(val));
        state += val;
    }
    return state.getValue();
}

REGISTER_STABLE_EXPRESSION(add, ExpressionAdd::parse);
const char* ExpressionAdd::getOpName() const {
    return "$add";
}

/* ------------------------- ExpressionAllElementsTrue -------------------------- */

Value ExpressionAllElementsTrue::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(allElementsTrue, ExpressionAllElementsTrue::parse);
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
    const size_t n = pAnd->_children.size();
    // ExpressionNary::optimize() generates an ExpressionConstant for {$and:[]}.
    MONGO_verify(n > 0);
    intrusive_ptr<Expression> pLast(pAnd->_children[n - 1]);
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
            ExpressionCoerceToBool::create(getExpressionContext(), std::move(pAnd->_children[0])));
        return pFinal;
    }

    /*
      Remove the final "true" value, and return the new expression.

      CW TODO:
      Note that because of any implicit conversions, we may need to
      apply an implicit boolean conversion.
    */
    pAnd->_children.resize(n - 1);
    return pE;
}

Value ExpressionAnd::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(and, ExpressionAnd::parse);
const char* ExpressionAnd::getOpName() const {
    return "$and";
}

/* ------------------------- ExpressionAnyElementTrue -------------------------- */

Value ExpressionAnyElementTrue::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(anyElementTrue, ExpressionAnyElementTrue::parse);
const char* ExpressionAnyElementTrue::getOpName() const {
    return "$anyElementTrue";
}

/* ---------------------- ExpressionArray --------------------------- */

Value ExpressionArray::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

Value ExpressionArray::serialize(const SerializationOptions& options) const {
    if (options.literalPolicy != LiteralSerializationPolicy::kUnchanged &&
        selfAndChildrenAreConstant()) {
        return ExpressionConstant::serializeConstant(
            options, evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    vector<Value> expressions;
    expressions.reserve(_children.size());
    for (auto&& expr : _children) {
        expressions.push_back(expr->serialize(options));
    }
    return Value(std::move(expressions));
}

intrusive_ptr<Expression> ExpressionArray::optimize() {
    bool allValuesConstant = true;

    for (auto&& expr : _children) {
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

bool ExpressionArray::selfAndChildrenAreConstant() const {
    for (auto&& exprPointer : _children) {
        if (!exprPointer->selfAndChildrenAreConstant()) {
            return false;
        }
    }
    return true;
}

const char* ExpressionArray::getOpName() const {
    // This should never be called, but is needed to inherit from ExpressionNary.
    return "$array";
}

/* ------------------------- ExpressionArrayElemAt -------------------------- */

Value ExpressionArrayElemAt::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(arrayElemAt, ExpressionArrayElemAt::parse);
const char* ExpressionArrayElemAt::getOpName() const {
    return "$arrayElemAt";
}

/* ------------------------- ExpressionFirst -------------------------- */

Value ExpressionFirst::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(first, ExpressionFirst::parse);

const char* ExpressionFirst::getOpName() const {
    return "$first";
}

/* ------------------------- ExpressionLast -------------------------- */

Value ExpressionLast::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(last, ExpressionLast::parse);

const char* ExpressionLast::getOpName() const {
    return "$last";
}

/* ------------------------- ExpressionObjectToArray -------------------------- */

Value ExpressionObjectToArray::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(objectToArray, ExpressionObjectToArray::parse);
const char* ExpressionObjectToArray::getOpName() const {
    return "$objectToArray";
}

/* ------------------------- ExpressionArrayToObject -------------------------- */
Value ExpressionArrayToObject::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(arrayToObject, ExpressionArrayToObject::parse);
const char* ExpressionArrayToObject::getOpName() const {
    return "$arrayToObject";
}

/* ------------------------- ExpressionBsonSize -------------------------- */

REGISTER_STABLE_EXPRESSION(bsonSize, ExpressionBsonSize::parse);

Value ExpressionBsonSize::evaluate(const Document& root, Variables* variables) const {
    Value arg = _children[0]->evaluate(root, variables);

    if (arg.nullish())
        return Value(BSONNULL);

    uassert(31393,
            str::stream() << "$bsonSize requires a document input, found: "
                          << typeName(arg.getType()),
            arg.getType() == BSONType::Object);

    return Value(arg.getDocument().toBson().objsize());
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

REGISTER_STABLE_EXPRESSION(ceil, ExpressionCeil::parse);
const char* ExpressionCeil::getOpName() const {
    return "$ceil";
}

/* -------------------- ExpressionCoerceToBool ------------------------- */

intrusive_ptr<ExpressionCoerceToBool> ExpressionCoerceToBool::create(
    ExpressionContext* const expCtx, intrusive_ptr<Expression> pExpression) {
    return new ExpressionCoerceToBool(expCtx, std::move(pExpression));
}

ExpressionCoerceToBool::ExpressionCoerceToBool(ExpressionContext* const expCtx,
                                               intrusive_ptr<Expression> pExpression)
    : Expression(expCtx, {std::move(pExpression)}) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

intrusive_ptr<Expression> ExpressionCoerceToBool::optimize() {
    /* optimize the operand */
    _children[_kExpression] = _children[_kExpression]->optimize();

    /* if the operand already produces a boolean, then we don't need this */
    /* LATER - Expression to support a "typeof" query? */
    Expression* pE = _children[_kExpression].get();
    if (dynamic_cast<ExpressionAnd*>(pE) || dynamic_cast<ExpressionOr*>(pE) ||
        dynamic_cast<ExpressionNot*>(pE) || dynamic_cast<ExpressionCoerceToBool*>(pE))
        return _children[_kExpression];

    return intrusive_ptr<Expression>(this);
}

Value ExpressionCoerceToBool::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

Value ExpressionCoerceToBool::serialize(const SerializationOptions& options) const {
    // When not explaining, serialize to an $and expression. When parsed, the $and expression
    // will be optimized back into a ExpressionCoerceToBool.
    const char* name = options.verbosity ? "$coerceToBool" : "$and";
    return Value(DOC(name << DOC_ARRAY(_children[_kExpression]->serialize(options))));
}

/* ----------------------- ExpressionCompare --------------------------- */

namespace {
struct BoundOp {
    ExpressionCompare::CmpOp op;

    auto operator()(ExpressionContext* const expCtx,
                    BSONElement bsonExpr,
                    const VariablesParseState& vps) const {
        return ExpressionCompare::parse(expCtx, std::move(bsonExpr), vps, op);
    }
};
}  // namespace

REGISTER_STABLE_EXPRESSION(cmp, BoundOp{ExpressionCompare::CMP});
REGISTER_STABLE_EXPRESSION(eq, BoundOp{ExpressionCompare::EQ});
REGISTER_STABLE_EXPRESSION(gt, BoundOp{ExpressionCompare::GT});
REGISTER_STABLE_EXPRESSION(gte, BoundOp{ExpressionCompare::GTE});
REGISTER_STABLE_EXPRESSION(lt, BoundOp{ExpressionCompare::LT});
REGISTER_STABLE_EXPRESSION(lte, BoundOp{ExpressionCompare::LTE});
REGISTER_STABLE_EXPRESSION(ne, BoundOp{ExpressionCompare::NE});

intrusive_ptr<Expression> ExpressionCompare::parse(ExpressionContext* const expCtx,
                                                   BSONElement bsonExpr,
                                                   const VariablesParseState& vps,
                                                   CmpOp op) {
    intrusive_ptr<ExpressionCompare> expr = new ExpressionCompare(expCtx, op);
    ExpressionVector args = parseArguments(expCtx, bsonExpr, vps);
    expr->validateArguments(args);
    expr->_children = std::move(args);
    return expr;
}

boost::intrusive_ptr<ExpressionCompare> ExpressionCompare::create(
    ExpressionContext* const expCtx,
    CmpOp cmpOp,
    const boost::intrusive_ptr<Expression>& exprLeft,
    const boost::intrusive_ptr<Expression>& exprRight) {
    boost::intrusive_ptr<ExpressionCompare> expr = new ExpressionCompare(expCtx, cmpOp);
    expr->_children = {exprLeft, exprRight};
    return expr;
}

namespace {
// Lookup table for returning opName
struct CmpOpName {
    const char name[5];  // string name with trailing '\0'
};
static const CmpOpName cmpOpNames[7] = {
    /* EQ  */ {"$eq"},
    /* NE  */ {"$ne"},
    /* GT  */ {"$gt"},
    /* GTE */ {"$gte"},
    /* LT  */ {"$lt"},
    /* LTE */ {"$lte"},
    /* CMP */ {"$cmp"},
};
}  // namespace

Value ExpressionCompare::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

const char* ExpressionCompare::getOpName() const {
    return cmpOpNames[cmpOp].name;
}

/* ------------------------- ExpressionConcat ----------------------------- */

Value ExpressionConcat::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(concat, ExpressionConcat::parse);
const char* ExpressionConcat::getOpName() const {
    return "$concat";
}

/* ------------------------- ExpressionConcatArrays ----------------------------- */

Value ExpressionConcatArrays::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(concatArrays, ExpressionConcatArrays::parse);
const char* ExpressionConcatArrays::getOpName() const {
    return "$concatArrays";
}

/* ----------------------- ExpressionCond ------------------------------ */

Value ExpressionCond::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionCond::optimize() {
    for (auto&& child : _children) {
        child = child->optimize();
    }

    if (auto ifOperand = dynamic_cast<ExpressionConstant*>(_children[0].get()); ifOperand) {
        return ifOperand->getValue().coerceToBool() ? _children[1] : _children[2];
    }

    return this;
}

boost::intrusive_ptr<Expression> ExpressionCond::create(ExpressionContext* const expCtx,
                                                        boost::intrusive_ptr<Expression> ifExp,
                                                        boost::intrusive_ptr<Expression> elseExpr,
                                                        boost::intrusive_ptr<Expression> thenExpr) {
    intrusive_ptr<ExpressionCond> ret = new ExpressionCond(expCtx);
    ret->_children.resize(3);

    ret->_children[0] = ifExp;
    ret->_children[1] = elseExpr;
    ret->_children[2] = thenExpr;
    return ret;
}

intrusive_ptr<Expression> ExpressionCond::parse(ExpressionContext* const expCtx,
                                                BSONElement expr,
                                                const VariablesParseState& vps) {
    if (expr.type() != Object) {
        return Base::parse(expCtx, expr, vps);
    }
    MONGO_verify(expr.fieldNameStringData() == "$cond");

    intrusive_ptr<ExpressionCond> ret = new ExpressionCond(expCtx);
    ret->_children.resize(3);

    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        if (arg.fieldNameStringData() == "if") {
            ret->_children[0] = parseOperand(expCtx, arg, vps);
        } else if (arg.fieldNameStringData() == "then") {
            ret->_children[1] = parseOperand(expCtx, arg, vps);
        } else if (arg.fieldNameStringData() == "else") {
            ret->_children[2] = parseOperand(expCtx, arg, vps);
        } else {
            uasserted(17083,
                      str::stream() << "Unrecognized parameter to $cond: " << arg.fieldName());
        }
    }

    uassert(17080, "Missing 'if' parameter to $cond", ret->_children[0]);
    uassert(17081, "Missing 'then' parameter to $cond", ret->_children[1]);
    uassert(17082, "Missing 'else' parameter to $cond", ret->_children[2]);

    return ret;
}

REGISTER_STABLE_EXPRESSION(cond, ExpressionCond::parse);
const char* ExpressionCond::getOpName() const {
    return "$cond";
}

/* ---------------------- ExpressionConstant --------------------------- */

intrusive_ptr<Expression> ExpressionConstant::parse(ExpressionContext* const expCtx,
                                                    BSONElement exprElement,
                                                    const VariablesParseState& vps) {
    return new ExpressionConstant(expCtx, Value(exprElement));
}


intrusive_ptr<ExpressionConstant> ExpressionConstant::create(ExpressionContext* const expCtx,
                                                             const Value& value) {
    intrusive_ptr<ExpressionConstant> pEC(new ExpressionConstant(expCtx, value));
    return pEC;
}

ExpressionConstant::ExpressionConstant(ExpressionContext* const expCtx, const Value& value)
    : Expression(expCtx), _value(value) {}


intrusive_ptr<Expression> ExpressionConstant::optimize() {
    /* nothing to do */
    return intrusive_ptr<Expression>(this);
}

Value ExpressionConstant::evaluate(const Document& root, Variables* variables) const {
    return _value;
}

Value ExpressionConstant::serialize(const SerializationOptions& options) const {
    return ExpressionConstant::serializeConstant(options, _value);
}

REGISTER_STABLE_EXPRESSION(const, ExpressionConstant::parse);
REGISTER_STABLE_EXPRESSION(literal, ExpressionConstant::parse);  // alias
const char* ExpressionConstant::getOpName() const {
    return "$const";
}

/* ---------------------- ExpressionDateFromParts ----------------------- */

REGISTER_STABLE_EXPRESSION(dateFromParts, ExpressionDateFromParts::parse);
intrusive_ptr<Expression> ExpressionDateFromParts::parse(ExpressionContext* const expCtx,
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
                      str::stream()
                          << "Unrecognized argument to $dateFromParts: " << arg.fieldName());
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

ExpressionDateFromParts::ExpressionDateFromParts(ExpressionContext* const expCtx,
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
    : Expression(expCtx,
                 {std::move(year),
                  std::move(month),
                  std::move(day),
                  std::move(hour),
                  std::move(minute),
                  std::move(second),
                  std::move(millisecond),
                  std::move(isoWeekYear),
                  std::move(isoWeek),
                  std::move(isoDayOfWeek),
                  std::move(timeZone)}) {}

intrusive_ptr<Expression> ExpressionDateFromParts::optimize() {
    if (_children[_kYear]) {
        _children[_kYear] = _children[_kYear]->optimize();
    }
    if (_children[_kMonth]) {
        _children[_kMonth] = _children[_kMonth]->optimize();
    }
    if (_children[_kDay]) {
        _children[_kDay] = _children[_kDay]->optimize();
    }
    if (_children[_kHour]) {
        _children[_kHour] = _children[_kHour]->optimize();
    }
    if (_children[_kMinute]) {
        _children[_kMinute] = _children[_kMinute]->optimize();
    }
    if (_children[_kSecond]) {
        _children[_kSecond] = _children[_kSecond]->optimize();
    }
    if (_children[_kMillisecond]) {
        _children[_kMillisecond] = _children[_kMillisecond]->optimize();
    }
    if (_children[_kIsoWeekYear]) {
        _children[_kIsoWeekYear] = _children[_kIsoWeekYear]->optimize();
    }
    if (_children[_kIsoWeek]) {
        _children[_kIsoWeek] = _children[_kIsoWeek]->optimize();
    }
    if (_children[_kIsoDayOfWeek]) {
        _children[_kIsoDayOfWeek] = _children[_kIsoDayOfWeek]->optimize();
    }
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_children[_kYear],
                                               _children[_kMonth],
                                               _children[_kDay],
                                               _children[_kHour],
                                               _children[_kMinute],
                                               _children[_kSecond],
                                               _children[_kMillisecond],
                                               _children[_kIsoWeekYear],
                                               _children[_kIsoWeek],
                                               _children[_kIsoDayOfWeek],
                                               _children[_kTimeZone]})) {

        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone =
            exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                           Document{},
                                           _children[_kTimeZone].get(),
                                           &(getExpressionContext()->variables));
        if (!_parsedTimeZone) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
    }

    return this;
}

Value ExpressionDateFromParts::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {"$dateFromParts",
         Document{
             {"year", _children[_kYear] ? _children[_kYear]->serialize(options) : Value()},
             {"month", _children[_kMonth] ? _children[_kMonth]->serialize(options) : Value()},
             {"day", _children[_kDay] ? _children[_kDay]->serialize(options) : Value()},
             {"hour", _children[_kHour] ? _children[_kHour]->serialize(options) : Value()},
             {"minute", _children[_kMinute] ? _children[_kMinute]->serialize(options) : Value()},
             {"second", _children[_kSecond] ? _children[_kSecond]->serialize(options) : Value()},
             {"millisecond",
              _children[_kMillisecond] ? _children[_kMillisecond]->serialize(options) : Value()},
             {"isoWeekYear",
              _children[_kIsoWeekYear] ? _children[_kIsoWeekYear]->serialize(options) : Value()},
             {"isoWeek", _children[_kIsoWeek] ? _children[_kIsoWeek]->serialize(options) : Value()},
             {"isoDayOfWeek",
              _children[_kIsoDayOfWeek] ? _children[_kIsoDayOfWeek]->serialize(options) : Value()},
             {"timezone",
              _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value()}}}});
}

Value ExpressionDateFromParts::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ---------------------- ExpressionDateFromString --------------------- */

REGISTER_STABLE_EXPRESSION(dateFromString, ExpressionDateFromString::parse);
intrusive_ptr<Expression> ExpressionDateFromString::parse(ExpressionContext* const expCtx,
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
                      str::stream()
                          << "Unrecognized argument to $dateFromString: " << arg.fieldName());
        }
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

ExpressionDateFromString::ExpressionDateFromString(ExpressionContext* const expCtx,
                                                   intrusive_ptr<Expression> dateString,
                                                   intrusive_ptr<Expression> timeZone,
                                                   intrusive_ptr<Expression> format,
                                                   intrusive_ptr<Expression> onNull,
                                                   intrusive_ptr<Expression> onError)
    : Expression(expCtx,
                 {std::move(dateString),
                  std::move(timeZone),
                  std::move(format),
                  std::move(onNull),
                  std::move(onError)}) {}

intrusive_ptr<Expression> ExpressionDateFromString::optimize() {
    _children[_kDateString] = _children[_kDateString]->optimize();
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }

    if (_children[_kFormat]) {
        _children[_kFormat] = _children[_kFormat]->optimize();
    }

    if (_children[_kOnNull]) {
        _children[_kOnNull] = _children[_kOnNull]->optimize();
    }

    if (_children[_kOnError]) {
        _children[_kOnError] = _children[_kOnError]->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_children[_kDateString],
                                               _children[_kTimeZone],
                                               _children[_kFormat],
                                               _children[_kOnNull],
                                               _children[_kOnError]})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone =
            exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                           Document{},
                                           _children[_kTimeZone].get(),
                                           &(getExpressionContext()->variables));
    }
    return this;
}

Value ExpressionDateFromString::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {"$dateFromString",
         Document{
             {"dateString", _children[_kDateString]->serialize(options)},
             {"timezone",
              _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value()},
             {"format", _children[_kFormat] ? _children[_kFormat]->serialize(options) : Value()},
             {"onNull", _children[_kOnNull] ? _children[_kOnNull]->serialize(options) : Value()},
             {"onError",
              _children[_kOnError] ? _children[_kOnError]->serialize(options) : Value()}}}});
}

Value ExpressionDateFromString::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ---------------------- ExpressionDateToParts ----------------------- */

REGISTER_STABLE_EXPRESSION(dateToParts, ExpressionDateToParts::parse);
intrusive_ptr<Expression> ExpressionDateToParts::parse(ExpressionContext* const expCtx,
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
                      str::stream()
                          << "Unrecognized argument to $dateToParts: " << arg.fieldName());
        }
    }

    uassert(40522, "Missing 'date' parameter to $dateToParts", dateElem);

    return new ExpressionDateToParts(
        expCtx,
        parseOperand(expCtx, dateElem, vps),
        timeZoneElem ? parseOperand(expCtx, timeZoneElem, vps) : nullptr,
        isoDateElem ? parseOperand(expCtx, isoDateElem, vps) : nullptr);
}

ExpressionDateToParts::ExpressionDateToParts(ExpressionContext* const expCtx,
                                             intrusive_ptr<Expression> date,
                                             intrusive_ptr<Expression> timeZone,
                                             intrusive_ptr<Expression> iso8601)
    : Expression(expCtx, {std::move(date), std::move(timeZone), std::move(iso8601)}) {}

intrusive_ptr<Expression> ExpressionDateToParts::optimize() {
    _children[_kDate] = _children[_kDate]->optimize();
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }
    if (_children[_kIso8601]) {
        _children[_kIso8601] = _children[_kIso8601]->optimize();
    }

    if (ExpressionConstant::allNullOrConstant(
            {_children[_kDate], _children[_kIso8601], _children[_kTimeZone]})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone =
            exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                           Document{},
                                           _children[_kTimeZone].get(),
                                           &(getExpressionContext()->variables));
        if (!_parsedTimeZone) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
    }

    return this;
}

Value ExpressionDateToParts::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {"$dateToParts",
         Document{{"date", _children[_kDate]->serialize(options)},
                  {"timezone",
                   _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value()},
                  {"iso8601",
                   _children[_kIso8601] ? _children[_kIso8601]->serialize(options) : Value()}}}});
}

Value ExpressionDateToParts::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ---------------------- ExpressionDateToString ----------------------- */

REGISTER_STABLE_EXPRESSION(dateToString, ExpressionDateToString::parse);
intrusive_ptr<Expression> ExpressionDateToString::parse(ExpressionContext* const expCtx,
                                                        BSONElement expr,
                                                        const VariablesParseState& vps) {
    MONGO_verify(expr.fieldNameStringData() == "$dateToString");

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
                      str::stream()
                          << "Unrecognized argument to $dateToString: " << arg.fieldName());
        }
    }

    uassert(18628, "Missing 'date' parameter to $dateToString", !dateElem.eoo());

    return new ExpressionDateToString(expCtx,
                                      parseOperand(expCtx, dateElem, vps),
                                      formatElem ? parseOperand(expCtx, formatElem, vps) : nullptr,
                                      timeZoneElem ? parseOperand(expCtx, timeZoneElem, vps)
                                                   : nullptr,
                                      onNullElem ? parseOperand(expCtx, onNullElem, vps) : nullptr);
}

ExpressionDateToString::ExpressionDateToString(ExpressionContext* const expCtx,
                                               intrusive_ptr<Expression> date,
                                               intrusive_ptr<Expression> format,
                                               intrusive_ptr<Expression> timeZone,
                                               intrusive_ptr<Expression> onNull)
    : Expression(expCtx,
                 {std::move(format), std::move(date), std::move(timeZone), std::move(onNull)}) {}

intrusive_ptr<Expression> ExpressionDateToString::optimize() {
    _children[_kDate] = _children[_kDate]->optimize();
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }

    if (_children[_kOnNull]) {
        _children[_kOnNull] = _children[_kOnNull]->optimize();
    }

    if (_children[_kFormat]) {
        _children[_kFormat] = _children[_kFormat]->optimize();
    }

    if (ExpressionConstant::allNullOrConstant(
            {_children[_kDate], _children[_kFormat], _children[_kTimeZone], _children[_kOnNull]})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone =
            exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                           Document{},
                                           _children[_kTimeZone].get(),
                                           &(getExpressionContext()->variables));
    }

    return this;
}

Value ExpressionDateToString::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {"$dateToString",
         Document{
             {"date", _children[_kDate]->serialize(options)},
             {"format", _children[_kFormat] ? _children[_kFormat]->serialize(options) : Value()},
             {"timezone",
              _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value()},
             {"onNull",
              _children[_kOnNull] ? _children[_kOnNull]->serialize(options) : Value()}}}});
}

Value ExpressionDateToString::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ----------------------- ExpressionDateDiff ---------------------------- */

REGISTER_STABLE_EXPRESSION(dateDiff, ExpressionDateDiff::parse);

ExpressionDateDiff::ExpressionDateDiff(ExpressionContext* const expCtx,
                                       boost::intrusive_ptr<Expression> startDate,
                                       boost::intrusive_ptr<Expression> endDate,
                                       boost::intrusive_ptr<Expression> unit,
                                       boost::intrusive_ptr<Expression> timezone,
                                       boost::intrusive_ptr<Expression> startOfWeek)
    : Expression{expCtx,
                 {std::move(startDate),
                  std::move(endDate),
                  std::move(unit),
                  std::move(timezone),
                  std::move(startOfWeek)}} {}

boost::intrusive_ptr<Expression> ExpressionDateDiff::parse(ExpressionContext* const expCtx,
                                                           BSONElement expr,
                                                           const VariablesParseState& vps) {
    invariant(expr.fieldNameStringData() == "$dateDiff");
    uassert(5166301,
            "$dateDiff only supports an object as its argument",
            expr.type() == BSONType::Object);
    BSONElement startDateElement, endDateElement, unitElement, timezoneElement, startOfWeekElement;
    for (auto&& element : expr.embeddedObject()) {
        auto field = element.fieldNameStringData();
        if ("startDate"_sd == field) {
            startDateElement = element;
        } else if ("endDate"_sd == field) {
            endDateElement = element;
        } else if ("unit"_sd == field) {
            unitElement = element;
        } else if ("timezone"_sd == field) {
            timezoneElement = element;
        } else if ("startOfWeek"_sd == field) {
            startOfWeekElement = element;
        } else {
            uasserted(5166302,
                      str::stream()
                          << "Unrecognized argument to $dateDiff: " << element.fieldName());
        }
    }
    uassert(5166303, "Missing 'startDate' parameter to $dateDiff", startDateElement);
    uassert(5166304, "Missing 'endDate' parameter to $dateDiff", endDateElement);
    uassert(5166305, "Missing 'unit' parameter to $dateDiff", unitElement);
    return make_intrusive<ExpressionDateDiff>(
        expCtx,
        parseOperand(expCtx, startDateElement, vps),
        parseOperand(expCtx, endDateElement, vps),
        parseOperand(expCtx, unitElement, vps),
        timezoneElement ? parseOperand(expCtx, timezoneElement, vps) : nullptr,
        startOfWeekElement ? parseOperand(expCtx, startOfWeekElement, vps) : nullptr);
}

boost::intrusive_ptr<Expression> ExpressionDateDiff::optimize() {
    _children[_kStartDate] = _children[_kStartDate]->optimize();
    _children[_kEndDate] = _children[_kEndDate]->optimize();
    _children[_kUnit] = _children[_kUnit]->optimize();
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }
    if (_children[_kStartOfWeek]) {
        _children[_kStartOfWeek] = _children[_kStartOfWeek]->optimize();
    }
    if (ExpressionConstant::allNullOrConstant({_children[_kStartDate],
                                               _children[_kEndDate],
                                               _children[_kUnit],
                                               _children[_kTimeZone],
                                               _children[_kStartOfWeek]})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isConstant(_children[_kUnit])) {
        const Value unitValue =
            _children[_kUnit]->evaluate(Document{}, &(getExpressionContext()->variables));
        if (unitValue.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        _parsedUnit = exec::expression::parseTimeUnit(unitValue, "$dateDiff"_sd);
    }
    if (ExpressionConstant::isConstant(_children[_kStartOfWeek])) {
        const Value startOfWeekValue =
            _children[_kStartOfWeek]->evaluate(Document{}, &(getExpressionContext()->variables));
        if (startOfWeekValue.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        _parsedStartOfWeek =
            exec::expression::parseDayOfWeek(startOfWeekValue, "$dateDiff"_sd, "startOfWeek"_sd);
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone = exec::expression::addContextToAssertionException(
            [&]() {
                return exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                                      Document{},
                                                      _children[_kTimeZone].get(),
                                                      &(getExpressionContext()->variables));
            },
            "$dateDiff parameter 'timezone' value parsing failed"_sd);
        if (!_parsedTimeZone) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
    }
    return this;
};

Value ExpressionDateDiff::serialize(const SerializationOptions& options) const {
    return Value{Document{
        {"$dateDiff"_sd,
         Document{{"startDate"_sd, _children[_kStartDate]->serialize(options)},
                  {"endDate"_sd, _children[_kEndDate]->serialize(options)},
                  {"unit"_sd, _children[_kUnit]->serialize(options)},
                  {"timezone"_sd,
                   _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value{}},
                  {"startOfWeek"_sd,
                   _children[_kStartOfWeek] ? _children[_kStartOfWeek]->serialize(options)
                                            : Value{}}}}}};
};

Value ExpressionDateDiff::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

monotonic::State ExpressionDateDiff::getMonotonicState(const FieldPath& sortedFieldPath) const {
    if (!ExpressionConstant::allNullOrConstant(
            {_children[_kUnit], _children[_kTimeZone], _children[_kStartOfWeek]})) {
        return monotonic::State::NonMonotonic;
    }
    // Because the result of this expression can be negative, this works the same way as
    // ExpressionSubtract. Edge cases with DST and other timezone changes are handled correctly
    // according to dateDiff.
    return monotonic::combine(
        _children[_kEndDate]->getMonotonicState(sortedFieldPath),
        monotonic::opposite(_children[_kStartDate]->getMonotonicState(sortedFieldPath)));
}

/* ----------------------- ExpressionDivide ---------------------------- */

Value ExpressionDivide::evaluate(const Document& root, Variables* variables) const {
    return uassertStatusOK(
        apply(_children[0]->evaluate(root, variables), _children[1]->evaluate(root, variables)));
}

StatusWith<Value> ExpressionDivide::apply(Value lhs, Value rhs) {
    if (lhs.numeric() && rhs.numeric()) {
        // If, and only if, either side is decimal, return decimal.
        if (lhs.getType() == NumberDecimal || rhs.getType() == NumberDecimal) {
            Decimal128 numer = lhs.coerceToDecimal();
            Decimal128 denom = rhs.coerceToDecimal();
            if (denom.isZero())
                return Status(ErrorCodes::BadValue, "can't $divide by zero");
            return Value(numer.divide(denom));
        }

        double numer = lhs.coerceToDouble();
        double denom = rhs.coerceToDouble();
        if (denom == 0.0)
            return Status(ErrorCodes::BadValue, "can't $divide by zero");

        return Value(numer / denom);
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "$divide only supports numeric types, not " << typeName(lhs.getType())
                          << " and " << typeName(rhs.getType()));
    }
}

REGISTER_STABLE_EXPRESSION(divide, ExpressionDivide::parse);
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

REGISTER_STABLE_EXPRESSION(exp, ExpressionExp::parse);
const char* ExpressionExp::getOpName() const {
    return "$exp";
}

/* ---------------------- ExpressionObject --------------------------- */

ExpressionObject::ExpressionObject(ExpressionContext* const expCtx,
                                   std::vector<boost::intrusive_ptr<Expression>> _children,
                                   vector<pair<string, intrusive_ptr<Expression>&>>&& expressions)
    : Expression(expCtx, std::move(_children)), _expressions(std::move(expressions)) {}

boost::intrusive_ptr<ExpressionObject> ExpressionObject::create(
    ExpressionContext* const expCtx,
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>>&&
        expressionsWithChildrenInPlace) {
    std::vector<boost::intrusive_ptr<Expression>> children;
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>> expressions;
    for (auto& [unused, expression] : expressionsWithChildrenInPlace)
        // These 'push_back's must complete before we insert references to the 'children' vector
        // into the 'expressions' vector since 'push_back' invalidates references.
        children.push_back(std::move(expression));
    std::vector<boost::intrusive_ptr<Expression>>::size_type index = 0;
    for (auto& [fieldName, unused] : expressionsWithChildrenInPlace) {
        expressions.emplace_back(fieldName, children[index]);
        ++index;
    }
    // It is safe to 'std::move' 'children' since the standard guarantees the references are stable.
    return new ExpressionObject(expCtx, std::move(children), std::move(expressions));
}

intrusive_ptr<ExpressionObject> ExpressionObject::parse(ExpressionContext* const expCtx,
                                                        BSONObj obj,
                                                        const VariablesParseState& vps) {
    // Make sure we don't have any duplicate field names.
    stdx::unordered_set<string> specifiedFields;

    std::vector<boost::intrusive_ptr<Expression>> children;
    vector<pair<string, intrusive_ptr<Expression>&>> expressions;
    for (auto&& elem : obj) {
        // Make sure this element has a valid field name. Use StringData here so that we can detect
        // if the field name contains a null byte.
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(elem.fieldNameStringData()),
            "Consider using $getField or $setField for a field path with '.' or '$'.");

        auto fieldName = elem.fieldName();
        uassert(16406,
                str::stream() << "duplicate field name specified in object literal: "
                              << obj.toString(),
                specifiedFields.find(fieldName) == specifiedFields.end());
        specifiedFields.insert(fieldName);
        children.push_back(parseOperand(expCtx, elem, vps));
    }

    std::vector<boost::intrusive_ptr<Expression>>::size_type index = 0;
    for (auto&& elem : obj) {
        expressions.emplace_back(elem.fieldName(), children[index]);
        ++index;
    }

    return new ExpressionObject{expCtx, std::move(children), std::move(expressions)};
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

Value ExpressionObject::evaluate(const Document& root, Variables* variables) const {
    MutableDocument outputDoc;
    for (auto&& pair : _expressions) {
        outputDoc.addField(pair.first, pair.second->evaluate(root, variables));
    }
    return outputDoc.freezeToValue();
}

bool ExpressionObject::selfAndChildrenAreConstant() const {
    for (auto&& [_, exprPointer] : _expressions) {
        if (!exprPointer->selfAndChildrenAreConstant()) {
            return false;
        }
    }
    return true;
}

Value ExpressionObject::serialize(const SerializationOptions& options) const {
    if (options.literalPolicy != LiteralSerializationPolicy::kUnchanged &&
        selfAndChildrenAreConstant()) {
        return ExpressionConstant::serializeConstant(options, Value(Document{}));
    }
    MutableDocument outputDoc;
    for (auto&& pair : _expressions) {
        outputDoc.addField(options.serializeFieldPathFromString(pair.first),
                           pair.second->serialize(options));
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
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::deprecatedCreate(
    ExpressionContext* const expCtx, const string& fieldPath) {
    return new ExpressionFieldPath(expCtx, "CURRENT." + fieldPath, Variables::kRootId);
}

// this is the new version that supports every syntax
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::parse(ExpressionContext* const expCtx,
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
        variableValidation::validateNameForUserRead(varName);
        auto varId = vps.getVariable(varName);

        // If the variable we are parsing is a system variable, then indicate that we have seen it.
        if (!Variables::isUserDefinedVariable(varId)) {
            expCtx->setSystemVarReferencedInQuery(varId);
        }

        return new ExpressionFieldPath(expCtx, fieldPath.toString(), varId);
    } else {
        return new ExpressionFieldPath(expCtx,
                                       "CURRENT." + raw.substr(1),  // strip the "$" prefix
                                       vps.getVariable("CURRENT"));
    }
}

intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::createPathFromString(
    ExpressionContext* const expCtx, const string& raw, const VariablesParseState& vps) {
    return new ExpressionFieldPath(expCtx, "CURRENT." + raw, vps.getVariable("CURRENT"));
}
intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::createVarFromString(
    ExpressionContext* const expCtx, const string& raw, const VariablesParseState& vps) {
    const auto rawSD = StringData{raw};
    const StringData varName = rawSD.substr(0, rawSD.find('.'));
    auto varId = vps.getVariable(varName);
    return new ExpressionFieldPath(expCtx, raw, varId);
}

ExpressionFieldPath::ExpressionFieldPath(ExpressionContext* const expCtx,
                                         const string& theFieldPath,
                                         Variables::Id variable)
    : Expression(expCtx), _fieldPath(theFieldPath, true /*precomputeHashes*/), _variable(variable) {
    const auto varName = theFieldPath.substr(0, theFieldPath.find('.'));
    tassert(5943201,
            std::string{"Variable with $$ROOT's id is not $$CURRENT or $$ROOT as expected, "
                        "field path is actually '"} +
                theFieldPath + "'",
            _variable != Variables::kRootId || varName == "CURRENT" || varName == "ROOT");
}

intrusive_ptr<Expression> ExpressionFieldPath::optimize() {
    if (_variable == Variables::kRemoveId) {
        // The REMOVE system variable optimizes to a constant missing value.
        return ExpressionConstant::create(getExpressionContext(), Value());
    }

    const bool sbeFullEnabled = feature_flags::gFeatureFlagSbeFull.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    if (sbeFullEnabled &&
        (_variable == Variables::kNowId || _variable == Variables::kClusterTimeId ||
         _variable == Variables::kUserRolesId)) {
        // Normally, we should be able to constant fold ExpressionFieldPath representing a system
        // variable into an ExpressionConstant during expression optimization. However, this causes
        // a problem with the current implementation of the SBE plan cache as it would effectively
        // embed a value for the system variable into the plan in the cache. This constant folding
        // optimization is important for queries which want to use an index scan and contain a
        // predicate referencing a system variable, for example:
        // {a: {$expr: {$lt: ["$foo", {$subtract: ["$$NOW", 10]}]}}})
        // Saving such a plan with the constant folded expression in the plan is wrong because a
        // cache hit will reuse the plan with the wrong constant, resulting in incorrect query
        // results. To avoid this problem, when featureFlagSbeFull is enabled (the SBE plan cache is
        // enabled), prohibit this optimization.
        return intrusive_ptr<Expression>(this);
    }

    // We allow system variables to be constant folded when the SBE plan cache is not enabled.
    if (getExpressionContext()->variables.hasConstantValue(_variable)) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables)));
    }

    return intrusive_ptr<Expression>(this);
}

bool ExpressionFieldPath::representsPath(const std::string& dottedPath) const {
    if (_variable != Variables::kRootId || _fieldPath.getPathLength() == 1) {
        // This variable refers to the entire document, or refers to a sub-field of something
        // besides the root document. Either way we can't prove that it represents the path given by
        // 'dottedPath'.
        return false;
    }
    return _fieldPath.tail().fullPath() == dottedPath;
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
        return input[_fieldPath.getFieldNameHashed(index)];

    // Try to dive deeper
    const Value val = input[_fieldPath.getFieldNameHashed(index)];
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

namespace {
// Shared among expressions that need to serialize dotted paths and redact the path components.
auto getPrefixAndPath(FieldPath path) {
    if (path.getFieldName(0) == "CURRENT" && path.getPathLength() > 1) {
        // use short form for "$$CURRENT.foo" but not just "$$CURRENT"
        return std::make_pair(std::string("$"), path.tail());
    } else {
        return std::make_pair(std::string("$$"), path);
    }
}
}  // namespace

Value ExpressionFieldPath::serialize(const SerializationOptions& options) const {
    auto [prefix, path] = getPrefixAndPath(_fieldPath);
    // First handles special cases for redaction of system variables. User variables will fall
    // through to the default full redaction case.
    if (options.transformIdentifiers && prefix.length() == 2) {
        if (path.getPathLength() == 1 && Variables::isBuiltin(_variable)) {
            // Nothing to redact for builtin variables.
            return Value(prefix + path.fullPath());
        } else if (path.getPathLength() > 1 && Variables::isBuiltin(_variable)) {
            // The first component of this path is a system variable, so keep that and redact
            // the rest.
            return Value(prefix + path.front() + "." + options.serializeFieldPath(path.tail()));
        }
    }
    return Value(prefix + options.serializeFieldPath(path));
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
        // Add dotted renames also to complex renames, to be used prospectively in optimizations
        // (e.g., pushDotRenamedMatch).
        // We only include dotted rename paths of length 3, as current optimization are constrained
        // to accepting only such paths to avoid semantic errors from array flattening.
        if (_variable == renamingVar && _fieldPath.getPathLength() == 3u) {
            outputPaths.complexRenames[exprFieldPath] = _fieldPath.tail().fullPath();
        }

        outputPaths.paths.insert(exprFieldPath);
    }

    return outputPaths;
}

std::unique_ptr<Expression> ExpressionFieldPath::copyWithSubstitution(
    const StringMap<std::string>& renameList) const {
    if (_variable != Variables::kRootId || _fieldPath.getPathLength() == 1) {
        return nullptr;
    }

    FieldRef path(getFieldPathWithoutCurrentPrefix().fullPath());
    for (const auto& rename : renameList) {
        if (FieldRef oldName(rename.first); oldName.isPrefixOfOrEqualTo(path)) {
            // Remove the path components of 'oldName' from 'path'.
            auto suffix = (path == oldName)
                ? ""
                : "." + path.dottedSubstring(oldName.numParts(), path.numParts());
            return std::unique_ptr<Expression>(new ExpressionFieldPath(
                getExpressionContext(), "CURRENT." + rename.second + suffix, getVariableId()));
        }
    }
    return nullptr;
}

bool ExpressionFieldPath::isRenameableByAnyPrefixNameIn(
    const StringMap<std::string>& renameList) const {
    if (_variable != Variables::kRootId || _fieldPath.getPathLength() == 1) {
        return false;
    }

    FieldRef path(getFieldPathWithoutCurrentPrefix().fullPath());
    for (const auto& rename : renameList) {
        if (FieldRef oldName(rename.first); oldName.isPrefixOfOrEqualTo(path)) {
            return true;
        }
    }
    return false;
}

monotonic::State ExpressionFieldPath::getMonotonicState(const FieldPath& sortedFieldPath) const {
    return getFieldPathWithoutCurrentPrefix() == sortedFieldPath ? monotonic::State::Increasing
                                                                 : monotonic::State::NonMonotonic;
}

/* ------------------------- ExpressionFilter ----------------------------- */

REGISTER_STABLE_EXPRESSION(filter, ExpressionFilter::parse);
intrusive_ptr<Expression> ExpressionFilter::parse(ExpressionContext* const expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn) {
    MONGO_verify(expr.fieldNameStringData() == "$filter");

    uassert(28646, "$filter only supports an object as its argument", expr.type() == Object);

    // "cond" must be parsed after "as" regardless of BSON order.
    BSONElement inputElem;
    BSONElement asElem;
    BSONElement condElem;
    BSONElement limitElem;


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
        } else {
            uasserted(28647,
                      str::stream() << "Unrecognized parameter to $filter: " << elem.fieldName());
        }
    }

    uassert(28648, "Missing 'input' parameter to $filter", !inputElem.eoo());
    uassert(28650, "Missing 'cond' parameter to $filter", !condElem.eoo());

    // Parse "input", only has outer variables.
    intrusive_ptr<Expression> input = parseOperand(expCtx, inputElem, vpsIn);

    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our variable, vpsIn doesn't.
    // Parse "as". If "as" is not specified, then use "this" by default.
    auto varName = asElem.eoo() ? "this" : asElem.str();

    variableValidation::validateNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // Parse "cond", has access to "as" variable.
    intrusive_ptr<Expression> cond = parseOperand(expCtx, condElem, vpsSub);

    if (limitElem) {
        intrusive_ptr<Expression> limit = parseOperand(expCtx, limitElem, vpsIn);
        return new ExpressionFilter(
            expCtx, std::move(varName), varId, std::move(input), std::move(cond), std::move(limit));
    }

    return new ExpressionFilter(
        expCtx, std::move(varName), varId, std::move(input), std::move(cond));
}

ExpressionFilter::ExpressionFilter(ExpressionContext* const expCtx,
                                   string varName,
                                   Variables::Id varId,
                                   intrusive_ptr<Expression> input,
                                   intrusive_ptr<Expression> cond,
                                   intrusive_ptr<Expression> limit)
    : Expression(expCtx,
                 limit ? makeVector(std::move(input), std::move(cond), std::move(limit))
                       : makeVector(std::move(input), std::move(cond))),
      _varName(std::move(varName)),
      _varId(varId),
      _limit(_children.size() == 3 ? 2 : boost::optional<size_t>(boost::none)) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

intrusive_ptr<Expression> ExpressionFilter::optimize() {
    // TODO handle when _input is constant.
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kCond] = _children[_kCond]->optimize();
    if (_limit)
        _children[*_limit] = (_children[*_limit])->optimize();

    return this;
}

Value ExpressionFilter::serialize(const SerializationOptions& options) const {
    if (_limit) {
        return Value(DOC(
            "$filter" << DOC("input" << _children[_kInput]->serialize(options) << "as" << _varName
                                     << "cond" << _children[_kCond]->serialize(options) << "limit"
                                     << (_children[*_limit])->serialize(options))));
    }
    return Value(
        DOC("$filter" << DOC("input" << _children[_kInput]->serialize(options) << "as" << _varName
                                     << "cond" << _children[_kCond]->serialize(options))));
}

Value ExpressionFilter::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ------------------------- ExpressionFloor -------------------------- */

StatusWith<Value> ExpressionFloor::apply(Value arg) {
    if (!arg.numeric()) {
        return Status{ErrorCodes::Error(5733411), "Floor must take a numeric argument"};
    }
    switch (arg.getType()) {
        case NumberDouble:
            return Value(std::floor(arg.getDouble()));
        case NumberDecimal:
            // Round toward the nearest decimal with a zero exponent in the negative direction.
            return Value(arg.getDecimal().quantize(Decimal128::kNormalizedZero,
                                                   Decimal128::kRoundTowardNegative));
        default:
            return arg;
    }
}

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

REGISTER_STABLE_EXPRESSION(floor, ExpressionFloor::parse);
const char* ExpressionFloor::getOpName() const {
    return "$floor";
}

/* ------------------------- ExpressionMap ----------------------------- */

REGISTER_STABLE_EXPRESSION(map, ExpressionMap::parse);
intrusive_ptr<Expression> ExpressionMap::parse(ExpressionContext* const expCtx,
                                               BSONElement expr,
                                               const VariablesParseState& vpsIn) {
    MONGO_verify(expr.fieldNameStringData() == "$map");

    uassert(16878, "$map only supports an object as its argument", expr.type() == Object);

    // "in" must be parsed after "as" regardless of BSON order
    BSONElement inputElem;
    BSONElement asElem;
    BSONElement inElem;
    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        if (arg.fieldNameStringData() == "input") {
            inputElem = arg;
        } else if (arg.fieldNameStringData() == "as") {
            asElem = arg;
        } else if (arg.fieldNameStringData() == "in") {
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

    variableValidation::validateNameForUserWrite(varName);
    Variables::Id varId = vpsSub.defineVariable(varName);

    // parse "in"
    intrusive_ptr<Expression> in =
        parseOperand(expCtx, inElem, vpsSub);  // has access to map variable

    return new ExpressionMap(expCtx, varName, varId, input, in);
}

ExpressionMap::ExpressionMap(ExpressionContext* const expCtx,
                             const string& varName,
                             Variables::Id varId,
                             intrusive_ptr<Expression> input,
                             intrusive_ptr<Expression> each)
    : Expression(expCtx, {std::move(input), std::move(each)}), _varName(varName), _varId(varId) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

intrusive_ptr<Expression> ExpressionMap::optimize() {
    // TODO handle when _input is constant
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kEach] = _children[_kEach]->optimize();
    return this;
}

Value ExpressionMap::serialize(const SerializationOptions& options) const {
    return Value(
        DOC("$map" << DOC("input" << _children[_kInput]->serialize(options) << "as" << _varName
                                  << "in" << _children[_kEach]->serialize(options))));
}

Value ExpressionMap::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ------------------------- ExpressionMeta ----------------------------- */

using MetaType = DocumentMetadataFields::MetaType;

REGISTER_EXPRESSION_CONDITIONALLY(meta,
                                  ExpressionMeta::parse,
                                  AllowedWithApiStrict::kConditionally,
                                  AllowedWithClientType::kAny,
                                  boost::none,
                                  true);

void ExpressionMeta::_assertMetaFieldCompatibleWithStrictAPI(ExpressionContext* const expCtx,
                                                             MetaType type) {
    const bool apiStrict = expCtx->getOperationContext() &&
        APIParameters::get(expCtx->getOperationContext()).getAPIStrict().value_or(false);
    // TODO SERVER-97104: add 'scoreDetails' here.
    static const std::set<MetaType> kUnstableMetaFields = {MetaType::kSearchScore,
                                                           MetaType::kIndexKey,
                                                           MetaType::kTextScore,
                                                           MetaType::kSearchHighlights,
                                                           MetaType::kSearchSequenceToken,
                                                           MetaType::kScore};
    const bool usesUnstableField = kUnstableMetaFields.contains(type);
    uassert(ErrorCodes::APIStrictError,
            str::stream() << "Provided apiStrict is true with an unstable meta field \""
                          << DocumentMetadataFields::serializeMetaType(type) << "\"",
            !apiStrict || !usesUnstableField);
}

void ExpressionMeta::_assertMetaFieldCompatibleWithHybridScoringFF(ExpressionContext* const expCtx,
                                                                   MetaType type) {
    // TODO SERVER-97104: add 'scoreDetails' here.
    static const std::set<MetaType> kHybridScoringProtectedFields = {MetaType::kScore};
    const bool usesHybridScoringProtectedField = kHybridScoringProtectedFields.contains(type);
    const bool hybridScoringFFEnabled =
        feature_flags::gFeatureFlagSearchHybridScoringPrerequisites
            .isEnabledUseLastLTSFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    uassert(ErrorCodes::FailedToParse,
            "'featureFlagSearchHybridScoringPrerequisites' must be enabled to use "
            "this meta field",
            !usesHybridScoringProtectedField || hybridScoringFFEnabled);
}

intrusive_ptr<Expression> ExpressionMeta::parse(ExpressionContext* const expCtx,
                                                BSONElement expr,
                                                const VariablesParseState& vpsIn) {
    uassert(17307, "$meta only supports string arguments", expr.type() == String);

    const auto typeName = expr.valueStringData();
    // parseMetaType() validates by throwing a uassert if typeName is an invalid meta type name.
    const auto metaType = DocumentMetadataFields::parseMetaType(typeName);

    _assertMetaFieldCompatibleWithStrictAPI(expCtx, metaType);
    _assertMetaFieldCompatibleWithHybridScoringFF(expCtx, metaType);
    return new ExpressionMeta(expCtx, metaType);
}

ExpressionMeta::ExpressionMeta(ExpressionContext* const expCtx, MetaType metaType)
    : Expression(expCtx), _metaType(metaType) {
    switch (_metaType) {
        case MetaType::kSearchScore:
        case MetaType::kSearchHighlights:
        case MetaType::kSearchScoreDetails:
        case MetaType::kSearchSequenceToken:
            break;
        default:
            // If the query contains $meta fields that are not currently supported by SBE, then
            // we can't run any part of pipeline in SBE and we have to run the entire pipeline
            // under the classic engine.
            expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
            expCtx->setSbePipelineCompatibility(SbeCompatibility::notCompatible);
    }
}

Value ExpressionMeta::serialize(const SerializationOptions& options) const {
    return Value(DOC("$meta" << DocumentMetadataFields::serializeMetaType(_metaType)));
}

Value ExpressionMeta::evaluate(const Document& root, Variables* variables) const {
    const auto& metadata = root.metadata();
    switch (_metaType) {
        case MetaType::kScore:
            return metadata.hasScore() ? Value(metadata.getScore()) : Value();
        case MetaType::kVectorSearchScore:
            return metadata.hasVectorSearchScore() ? Value(metadata.getVectorSearchScore())
                                                   : Value();
        case MetaType::kTextScore:
            return metadata.hasTextScore() ? Value(metadata.getTextScore()) : Value();
        case MetaType::kRandVal:
            return metadata.hasRandVal() ? Value(metadata.getRandVal()) : Value();
        case MetaType::kSearchScore:
            return metadata.hasSearchScore() ? Value(metadata.getSearchScore()) : Value();
        case MetaType::kSearchHighlights:
            return metadata.hasSearchHighlights() ? Value(metadata.getSearchHighlights()) : Value();
        case MetaType::kGeoNearDist:
            return metadata.hasGeoNearDistance() ? Value(metadata.getGeoNearDistance()) : Value();
        case MetaType::kGeoNearPoint:
            return metadata.hasGeoNearPoint() ? Value(metadata.getGeoNearPoint()) : Value();
        case MetaType::kRecordId: {
            // Be sure that a RecordId can be represented by a long long.
            static_assert(RecordId::kMinRepr >= std::numeric_limits<long long>::min());
            static_assert(RecordId::kMaxRepr <= std::numeric_limits<long long>::max());
            if (!metadata.hasRecordId()) {
                return Value();
            }

            BSONObjBuilder builder;
            metadata.getRecordId().serializeToken("", &builder);
            return Value(builder.done().firstElement());
        }
        case MetaType::kIndexKey:
            return metadata.hasIndexKey() ? Value(metadata.getIndexKey()) : Value();
        case MetaType::kSortKey:
            return metadata.hasSortKey()
                ? Value(DocumentMetadataFields::serializeSortKey(metadata.isSingleElementKey(),
                                                                 metadata.getSortKey()))
                : Value();
        case MetaType::kSearchScoreDetails:
            return metadata.hasSearchScoreDetails() ? Value(metadata.getSearchScoreDetails())
                                                    : Value();
        case MetaType::kSearchSequenceToken:
            return metadata.hasSearchSequenceToken() ? Value(metadata.getSearchSequenceToken())
                                                     : Value();
        case MetaType::kTimeseriesBucketMinTime:
            return metadata.hasTimeseriesBucketMinTime()
                ? Value(metadata.getTimeseriesBucketMinTime())
                : Value();
        case MetaType::kTimeseriesBucketMaxTime:
            return metadata.hasTimeseriesBucketMaxTime()
                ? Value(metadata.getTimeseriesBucketMaxTime())
                : Value();
        default:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

/* ----------------------- ExpressionMod ---------------------------- */

StatusWith<Value> ExpressionMod::apply(Value lhs, Value rhs) {
    BSONType leftType = lhs.getType();
    BSONType rightType = rhs.getType();

    if (lhs.numeric() && rhs.numeric()) {

        // If either side is decimal, perform the operation in decimal.
        if (leftType == NumberDecimal || rightType == NumberDecimal) {
            Decimal128 left = lhs.coerceToDecimal();
            Decimal128 right = rhs.coerceToDecimal();
            if (right.isZero()) {
                return Status(ErrorCodes::Error(5733415), str::stream() << "can't $mod by zero");
            }

            return Value(left.modulo(right));
        }

        // ensure we aren't modding by 0
        double right = rhs.coerceToDouble();
        if (right == 0) {
            return Status(ErrorCodes::Error(16610), str::stream() << "can't $mod by zero");
        };

        if (leftType == NumberDouble || rightType == NumberDouble) {
            double left = lhs.coerceToDouble();
            return Value(fmod(left, right));
        }

        if (leftType == NumberLong || rightType == NumberLong) {
            // if either is long, return long
            long long left = lhs.coerceToLong();
            long long rightLong = rhs.coerceToLong();
            return Value(overflow::safeMod(left, rightLong));
        }

        // lastly they must both be ints, return int
        int left = lhs.coerceToInt();
        int rightInt = rhs.coerceToInt();
        return Value(overflow::safeMod(left, rightInt));
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else {
        return Status(ErrorCodes::Error(16611),
                      str::stream()
                          << "$mod only supports numeric types, not " << typeName(lhs.getType())
                          << " and " << typeName(rhs.getType()));
    }
}
Value ExpressionMod::evaluate(const Document& root, Variables* variables) const {
    Value lhs = _children[0]->evaluate(root, variables);
    Value rhs = _children[1]->evaluate(root, variables);
    return uassertStatusOK(apply(lhs, rhs));
}

REGISTER_STABLE_EXPRESSION(mod, ExpressionMod::parse);
const char* ExpressionMod::getOpName() const {
    return "$mod";
}

/* ------------------------- ExpressionMultiply ----------------------------- */

namespace {
class MultiplyState {
    /**
     * We'll try to return the narrowest possible result value.  To do that without creating
     * intermediate Values, do the arithmetic for double and integral types in parallel, tracking
     * the current narrowest type.
     */
    double doubleProduct = 1;
    long long longProduct = 1;
    Decimal128 decimalProduct;  // This will be initialized on encountering the first decimal.
    BSONType productType = NumberInt;

public:
    void operator*=(const Value& val) {
        tassert(5423304, "MultiplyState::operator*= only supports numbers", val.numeric());

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
                    overflow::mul(longProduct, val.coerceToLong(), &longProduct)) {
                    // The multiplier is either Infinity or NaN, or the `longProduct` would
                    // have overflowed, so we're abandoning it.
                    productType = NumberDouble;
                }
            }
        }
    }

    Value getValue() const {
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
};

Status checkMultiplyNumeric(Value val) {
    if (!val.numeric())
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "$multiply only supports numeric types, not "
                                    << typeName(val.getType()));
    return Status::OK();
}
}  // namespace

StatusWith<Value> ExpressionMultiply::apply(Value lhs, Value rhs) {
    // evaluate() checks arguments left-to-right, short circuiting on the first null or non-number.
    // Imitate that behavior here.
    if (lhs.nullish())
        return Value(BSONNULL);
    if (Status s = checkMultiplyNumeric(lhs); !s.isOK())
        return s;
    if (rhs.nullish())
        return Value(BSONNULL);
    if (Status s = checkMultiplyNumeric(rhs); !s.isOK())
        return s;

    MultiplyState state;
    state *= lhs;
    state *= rhs;
    return state.getValue();
}
Value ExpressionMultiply::evaluate(const Document& root, Variables* variables) const {
    MultiplyState state;
    for (auto&& child : _children) {
        Value val = child->evaluate(root, variables);
        if (val.nullish())
            return Value(BSONNULL);
        uassertStatusOK(checkMultiplyNumeric(val));
        state *= val;
    }
    return state.getValue();
}

REGISTER_STABLE_EXPRESSION(multiply, ExpressionMultiply::parse);
const char* ExpressionMultiply::getOpName() const {
    return "$multiply";
}

/* ----------------------- ExpressionIfNull ---------------------------- */

void ExpressionIfNull::validateArguments(const ExpressionVector& args) const {
    uassert(1257300,
            str::stream() << "$ifNull needs at least two arguments, had: " << args.size(),
            args.size() >= 2);
}

Value ExpressionIfNull::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionIfNull::optimize() {
    bool allOperandsConst = true;
    for (auto& operand : _children) {
        operand = operand->optimize();
        if (!dynamic_cast<ExpressionConstant*>(operand.get())) {
            allOperandsConst = false;
        }
    }

    // If all the operands are constant expressions, collapse the expression into one constant
    // expression.
    if (allOperandsConst) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables)));
    }

    // Remove all null constants, unless it is the only child or it is the last parameter
    // (<replacement-expression-if-null>). If one of the operands is a non-null constant expression,
    // remove any operands that follow it.
    auto it = _children.begin();
    tassert(5868001,
            str::stream() << "$ifNull needs at least two arguments, had: " << _children.size(),
            _children.size() > 1);
    while (it != _children.end() - 1) {
        if (auto constExpression = dynamic_cast<ExpressionConstant*>(it->get())) {
            if (constExpression->getValue().nullish()) {
                it = _children.erase(it);
            } else {
                _children.erase(it + 1, _children.end());
                break;
            }
        } else {
            ++it;
        }
    }

    if (_children.size() == 1) {
        // Replace $ifNull with its only child.
        return _children[0];
    }
    return this;
}

const char* ExpressionIfNull::getOpName() const {
    return "$ifNull";
}

REGISTER_STABLE_EXPRESSION(ifNull, ExpressionIfNull::parse);

/* ----------------------- ExpressionIn ---------------------------- */

Value ExpressionIn::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(in, ExpressionIn::parse);
const char* ExpressionIn::getOpName() const {
    return "$in";
}

/* ----------------------- ExpressionIndexOfArray ------------------ */

Value ExpressionIndexOfArray::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

intrusive_ptr<Expression> ExpressionIndexOfArray::optimize() {
    // This will optimize all arguments to this expression.
    auto optimized = ExpressionNary::optimize();
    if (optimized.get() != this) {
        return optimized;
    }
    // If the input array is an ExpressionConstant we can optimize using a unordered_map instead of
    // an array.
    if (auto constantArray = dynamic_cast<ExpressionConstant*>(_children[0].get())) {
        const Value valueArray = constantArray->getValue();
        if (valueArray.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        uassert(50809,
                str::stream() << "First operand of $indexOfArray must be an array. First "
                              << "argument is of type: " << typeName(valueArray.getType()),
                valueArray.isArray());

        _parsedIndexMap = exec::expression::arrayToIndexMap(
            valueArray, getExpressionContext()->getValueComparator());
    }
    return this;
}

REGISTER_STABLE_EXPRESSION(indexOfArray, ExpressionIndexOfArray::parse);
const char* ExpressionIndexOfArray::getOpName() const {
    return "$indexOfArray";
}

/* ----------------------- ExpressionIndexOfBytes ------------------ */

Value ExpressionIndexOfBytes::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(indexOfBytes, ExpressionIndexOfBytes::parse);
const char* ExpressionIndexOfBytes::getOpName() const {
    return "$indexOfBytes";
}

/* ----------------------- ExpressionIndexOfCP --------------------- */

Value ExpressionIndexOfCP::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(indexOfCP, ExpressionIndexOfCP::parse);
const char* ExpressionIndexOfCP::getOpName() const {
    return "$indexOfCP";
}

/* ----------------------- ExpressionLn ---------------------------- */

Value ExpressionLn::evaluateNumericArg(const Value& numericArg) const {
    if (numericArg.getType() == NumberDecimal) {
        Decimal128 argDecimal = numericArg.getDecimal();
        if (argDecimal.isGreater(Decimal128::kNormalizedZero))
            return Value(argDecimal.naturalLogarithm());
        // Fall through for error case.
    }
    double argDouble = numericArg.coerceToDouble();
    uassert(28766,
            str::stream() << "$ln's argument must be a positive number, but is " << argDouble,
            argDouble > 0 || std::isnan(argDouble));
    return Value(std::log(argDouble));
}

REGISTER_STABLE_EXPRESSION(ln, ExpressionLn::parse);
const char* ExpressionLn::getOpName() const {
    return "$ln";
}

/* ----------------------- ExpressionLog ---------------------------- */

Value ExpressionLog::evaluate(const Document& root, Variables* variables) const {
    Value argVal = _children[0]->evaluate(root, variables);
    Value baseVal = _children[1]->evaluate(root, variables);
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

REGISTER_STABLE_EXPRESSION(log, ExpressionLog::parse);
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

REGISTER_STABLE_EXPRESSION(log10, ExpressionLog10::parse);
const char* ExpressionLog10::getOpName() const {
    return "$log10";
}

/* ----------------------- ExpressionInternalFLEEqual ---------------------------- */
constexpr auto kInternalFleEq = "$_internalFleEq"_sd;

ExpressionInternalFLEEqual::ExpressionInternalFLEEqual(ExpressionContext* const expCtx,
                                                       boost::intrusive_ptr<Expression> field,
                                                       ServerZerosEncryptionToken zerosToken)
    : Expression(expCtx, {std::move(field)}), _evaluatorV2({std::move(zerosToken)}) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

REGISTER_STABLE_EXPRESSION(_internalFleEq, ExpressionInternalFLEEqual::parse);

intrusive_ptr<Expression> ExpressionInternalFLEEqual::parse(ExpressionContext* const expCtx,
                                                            BSONElement expr,
                                                            const VariablesParseState& vps) {

    IDLParserContext ctx(kInternalFleEq);

    auto fleEq = InternalFleEqStructV2::parse(ctx, expr.Obj());

    auto fieldExpr = Expression::parseOperand(expCtx, fleEq.getField().getElement(), vps);

    auto serverTokenPair = fromEncryptedConstDataRange(fleEq.getServerZerosEncryptionToken());

    uassert(7399502,
            "Invalid server token",
            serverTokenPair.first == EncryptedBinDataType::kFLE2TransientRaw &&
                serverTokenPair.second.length() == sizeof(PrfBlock));

    return new ExpressionInternalFLEEqual(
        expCtx, std::move(fieldExpr), ServerZerosEncryptionToken::parse(serverTokenPair.second));
}

Value toValue(const std::array<std::uint8_t, 32>& buf) {
    auto vec = toEncryptedVector(EncryptedBinDataType::kFLE2TransientRaw, buf);
    return Value(BSONBinData(vec.data(), vec.size(), BinDataType::Encrypt));
}

Value ExpressionInternalFLEEqual::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {kInternalFleEq,
         Document{{"field", _children[0]->serialize(options)},
                  {"server", toValue((_evaluatorV2.zerosDecryptionTokens()[0]).asPrfBlock())}}}});
}

Value ExpressionInternalFLEEqual::evaluate(const Document& root, Variables* variables) const {
    auto fieldValue = _children[0]->evaluate(root, variables);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }

    return Value(_evaluatorV2.evaluate(
        fieldValue, EncryptedBinDataType::kFLE2EqualityIndexedValueV2, [](auto serverValue) {
            auto swParsedFields =
                FLE2IndexedEqualityEncryptedValueV2::parseAndValidateFields(serverValue);
            uassertStatusOK(swParsedFields.getStatus());
            std::vector<ConstDataRange> metadataBlocks;
            metadataBlocks.push_back(swParsedFields.getValue().metadataBlock);
            return metadataBlocks;
        }));
}

const char* ExpressionInternalFLEEqual::getOpName() const {
    return kInternalFleEq.rawData();
}

/* ----------------------- ExpressionInternalFLEBetween ---------------------------- */

constexpr auto kInternalFleBetween = "$_internalFleBetween"_sd;

ExpressionInternalFLEBetween::ExpressionInternalFLEBetween(
    ExpressionContext* const expCtx,
    boost::intrusive_ptr<Expression> field,
    std::vector<ServerZerosEncryptionToken> zerosTokens)
    : Expression(expCtx, {std::move(field)}), _evaluatorV2(std::move(zerosTokens)) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

REGISTER_STABLE_EXPRESSION(_internalFleBetween, ExpressionInternalFLEBetween::parse);

intrusive_ptr<Expression> ExpressionInternalFLEBetween::parse(ExpressionContext* const expCtx,
                                                              BSONElement expr,
                                                              const VariablesParseState& vps) {
    IDLParserContext ctx(kInternalFleBetween);

    auto fleBetween = InternalFleBetweenStructV2::parse(ctx, expr.Obj());

    auto fieldExpr = Expression::parseOperand(expCtx, fleBetween.getField().getElement(), vps);

    std::vector<ServerZerosEncryptionToken> serverZerosEncryptionTokens;
    serverZerosEncryptionTokens.reserve(fleBetween.getServerZerosEncryptionTokens().size());

    for (auto& elem : fleBetween.getServerZerosEncryptionTokens()) {
        auto [first, second] = fromEncryptedConstDataRange(elem);

        uassert(7399503,
                "Invalid ServerDerivedFromDataToken",
                first == EncryptedBinDataType::kFLE2TransientRaw &&
                    second.length() == sizeof(PrfBlock));

        serverZerosEncryptionTokens.emplace_back(PrfBlockfromCDR(second));
    }

    return new ExpressionInternalFLEBetween(
        expCtx, std::move(fieldExpr), std::move(serverZerosEncryptionTokens));
}

Value ExpressionInternalFLEBetween::serialize(const SerializationOptions& options) const {
    std::vector<Value> serverDerivedValues;
    serverDerivedValues.reserve(_evaluatorV2.zerosDecryptionTokens().size());
    for (auto& token : _evaluatorV2.zerosDecryptionTokens()) {
        serverDerivedValues.push_back(toValue(token.asPrfBlock()));
    }
    return Value(Document{{kInternalFleBetween,
                           Document{{"field", _children[0]->serialize(options)},
                                    {"server", Value(std::move(serverDerivedValues))}}}});
}

Value ExpressionInternalFLEBetween::evaluate(const Document& root, Variables* variables) const {
    auto fieldValue = _children[0]->evaluate(root, variables);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }

    return Value(_evaluatorV2.evaluate(
        fieldValue, EncryptedBinDataType::kFLE2RangeIndexedValueV2, [](auto serverValue) {
            auto swParsedFields =
                FLE2IndexedRangeEncryptedValueV2::parseAndValidateFields(serverValue);
            uassertStatusOK(swParsedFields.getStatus());
            return swParsedFields.getValue().metadataBlocks;
        }));
}

const char* ExpressionInternalFLEBetween::getOpName() const {
    return kInternalFleBetween.rawData();
}

/* ------------------------ ExpressionNary ----------------------------- */

/**
 * Optimize a general Nary expression.
 *
 * The optimization has the following properties:
 *   1) Optimize each of the operands.
 *   2) If the operator is fully associative, flatten internal operators of the same type. I.e.:
 *      A+B+(C+D)+E => A+B+C+D+E
 *   3) If the operator is commutative & associative, group all constant operands. For example:
 *      c1 + c2 + n1 + c3 + n2 => n1 + n2 + c1 + c2 + c3
 *   4) If the operator is fully associative, execute the operation over all the contiguous constant
 *      operands and replacing them by the result. For example: c1 + c2 + n1 + c3 + c4 + n5 =>
 *      c5 = c1 + c2, c6 = c3 + c4 => c5 + n1 + c6 + n5
 *   5) If the operand is left-associative, execute the operation over all contiguous constant
 *      operands that precede the first non-constant operand. For example: c1 + c2 + n1 + c3 + c4 +
 *      n2 => c5 = c1 + c2, c5 + n1 + c3 + c4 + n5
 *
 * It returns the optimized expression. It can be exactly the same expression, a modified version
 * of the same expression or a completely different expression.
 */
intrusive_ptr<Expression> ExpressionNary::optimize() {
    uint32_t constOperandCount = 0;

    for (auto& operand : _children) {
        operand = operand->optimize();
        if (dynamic_cast<ExpressionConstant*>(operand.get())) {
            ++constOperandCount;
        }
    }
    // If all the operands are constant expressions, collapse the expression into one constant
    // expression.
    if (constOperandCount == _children.size()) {
        return intrusive_ptr<Expression>(ExpressionConstant::create(
            getExpressionContext(), evaluate(Document(), &(getExpressionContext()->variables))));
    }

    // An operator cannot be left-associative and commutative, because left-associative
    // operators need to preserve their order-of-operations.
    invariant(!(getAssociativity() == Associativity::kLeft && isCommutative()));

    // If the expression is associative, we can collapse all the consecutive constant operands
    // into one by applying the expression to those consecutive constant operands. If the
    // expression is also commutative we can reorganize all the operands so that all of the
    // constant ones are together (arbitrarily at the back) and we can collapse all of them into
    // one. If the operation is left-associative, then we will stop folding constants together when
    // we see the first non-constant operand.
    if (getAssociativity() == Associativity::kFull || getAssociativity() == Associativity::kLeft) {
        ExpressionVector constExpressions;
        ExpressionVector optimizedOperands;
        for (size_t i = 0; i < _children.size();) {
            intrusive_ptr<Expression> operand = _children[i];
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
            if (nary && !strcmp(nary->getOpName(), getOpName()) &&
                nary->getAssociativity() == Associativity::kFull) {
                invariant(!nary->_children.empty());
                _children[i] = std::move(nary->_children[0]);
                _children.insert(
                    _children.begin() + i + 1, nary->_children.begin() + 1, nary->_children.end());
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
                    ExpressionVector childrenSave = std::move(_children);
                    _children = std::move(constExpressions);
                    optimizedOperands.emplace_back(ExpressionConstant::create(
                        getExpressionContext(),
                        evaluate(Document(), &(getExpressionContext()->variables))));
                    _children = std::move(childrenSave);
                } else {
                    optimizedOperands.insert(
                        optimizedOperands.end(), constExpressions.begin(), constExpressions.end());
                }
                constExpressions.clear();
            }
            optimizedOperands.push_back(operand);

            // If the expression is left-associative, break out of the loop since we should only
            // optimize until the first non-constant.
            if (getAssociativity() == Associativity::kLeft) {
                // Dump the remaining operands into the optimizedOperands vector that will become
                // the new _children vector.
                optimizedOperands.insert(
                    optimizedOperands.end(), _children.begin() + i + 1, _children.end());
                break;
            }
            ++i;
        }

        if (constExpressions.size() > 1) {
            _children = std::move(constExpressions);
            optimizedOperands.emplace_back(ExpressionConstant::create(
                getExpressionContext(),
                evaluate(Document(), &(getExpressionContext()->variables))));
        } else {
            optimizedOperands.insert(
                optimizedOperands.end(), constExpressions.begin(), constExpressions.end());
        }

        _children = std::move(optimizedOperands);
    }
    return this;
}

void ExpressionNary::addOperand(const intrusive_ptr<Expression>& pExpression) {
    _children.push_back(pExpression);
}

Value ExpressionNary::serialize(const SerializationOptions& options) const {
    const size_t nOperand = _children.size();
    vector<Value> array;
    /* build up the array */
    for (size_t i = 0; i < nOperand; i++) {
        // If this input is a constant, bypass the standard serialization that wraps the
        // representative value in $const. This does not lead to ambiguity for variadic operators
        // but avoids bloating the representative shape for operators that have many inputs.
        ExpressionConstant const* exprConst = dynamic_cast<ExpressionConstant*>(_children[i].get());
        if (exprConst) {
            array.push_back(exprConst->serializeConstant(
                options, exprConst->getValue(), false /* wrapRepresentativeValue */));
        } else {
            array.push_back(_children[i]->serialize(options));
        }
    }
    return Value(DOC(getOpName() << array));
}

/* ------------------------- ExpressionNot ----------------------------- */

Value ExpressionNot::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(not, ExpressionNot::parse);
const char* ExpressionNot::getOpName() const {
    return "$not";
}

/* -------------------------- ExpressionOr ----------------------------- */

Value ExpressionOr::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
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
    const size_t n = pOr->_children.size();
    // ExpressionNary::optimize() generates an ExpressionConstant for {$or:[]}.
    MONGO_verify(n > 0);
    intrusive_ptr<Expression> pLast(pOr->_children[n - 1]);
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
            ExpressionCoerceToBool::create(getExpressionContext(), std::move(pOr->_children[0])));
        return pFinal;
    }

    /*
      Remove the final "false" value, and return the new expression.
    */
    pOr->_children.resize(n - 1);
    return pE;
}

REGISTER_STABLE_EXPRESSION(or, ExpressionOr::parse);
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
}  // namespace

/* ----------------------- ExpressionPow ---------------------------- */

intrusive_ptr<Expression> ExpressionPow::create(ExpressionContext* const expCtx,
                                                Value base,
                                                Value exp) {
    intrusive_ptr<ExpressionPow> expr(new ExpressionPow(expCtx));
    expr->_children.push_back(ExpressionConstant::create(expr->getExpressionContext(), base));
    expr->_children.push_back(ExpressionConstant::create(expr->getExpressionContext(), exp));
    return expr;
}

Value ExpressionPow::evaluate(const Document& root, Variables* variables) const {
    Value baseVal = _children[0]->evaluate(root, variables);
    Value expVal = _children[1]->evaluate(root, variables);
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

REGISTER_STABLE_EXPRESSION(pow, ExpressionPow::parse);
const char* ExpressionPow::getOpName() const {
    return "$pow";
}

/* ------------------------- ExpressionRange ------------------------------ */

Value ExpressionRange::evaluate(const Document& root, Variables* variables) const {
    Value startVal(_children[0]->evaluate(root, variables));
    Value endVal(_children[1]->evaluate(root, variables));

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
    if (_children.size() == 3) {
        // A step was specified by the user.
        Value stepVal(_children[2]->evaluate(root, variables));

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
                          << "and cannot spill to disk. Memory limit: " << memLimit << " bytes",
            memNeeded < memLimit);

    std::vector<Value> output;

    while ((step > 0 ? current < end : current > end)) {
        output.emplace_back(static_cast<int>(current));
        current += step;
    }

    return Value(std::move(output));
}

REGISTER_STABLE_EXPRESSION(range, ExpressionRange::parse);
const char* ExpressionRange::getOpName() const {
    return "$range";
}

/* ------------------------ ExpressionReduce ------------------------------ */

REGISTER_STABLE_EXPRESSION(reduce, ExpressionReduce::parse);
intrusive_ptr<Expression> ExpressionReduce::parse(ExpressionContext* const expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps) {
    uassert(40075,
            str::stream() << "$reduce requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == Object);


    // vpsSub is used only to parse 'in', which must have access to $$this and $$value.
    VariablesParseState vpsSub(vps);
    auto thisVar = vpsSub.defineVariable("this");
    auto valueVar = vpsSub.defineVariable("value");

    boost::intrusive_ptr<Expression> input;
    boost::intrusive_ptr<Expression> initial;
    boost::intrusive_ptr<Expression> in;
    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "input") {
            input = parseOperand(expCtx, elem, vps);
        } else if (field == "initialValue") {
            initial = parseOperand(expCtx, elem, vps);
        } else if (field == "in") {
            in = parseOperand(expCtx, elem, vpsSub);
        } else {
            uasserted(40076, str::stream() << "$reduce found an unknown argument: " << field);
        }
    }

    uassert(40077, "$reduce requires 'input' to be specified", input);
    uassert(40078, "$reduce requires 'initialValue' to be specified", initial);
    uassert(40079, "$reduce requires 'in' to be specified", in);

    return new ExpressionReduce(
        expCtx, std::move(input), std::move(initial), std::move(in), thisVar, valueVar);
}

Value ExpressionReduce::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

intrusive_ptr<Expression> ExpressionReduce::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kInitial] = _children[_kInitial]->optimize();
    _children[_kIn] = _children[_kIn]->optimize();
    return this;
}

Value ExpressionReduce::serialize(const SerializationOptions& options) const {
    return Value(Document{{"$reduce",
                           Document{{"input", _children[_kInput]->serialize(options)},
                                    {"initialValue", _children[_kInitial]->serialize(options)},
                                    {"in", _children[_kIn]->serialize(options)}}}});
}

/* ------------------------ ExpressionReplaceBase ------------------------ */

Value ExpressionReplaceBase::serialize(const SerializationOptions& options) const {
    return Value(
        Document{{getOpName(),
                  Document{{"input", _children[_kInput]->serialize(options)},
                           {"find", _children[_kFind]->serialize(options)},
                           {"replacement", _children[_kReplacement]->serialize(options)}}}});
}

namespace {
std::tuple<intrusive_ptr<Expression>, intrusive_ptr<Expression>, intrusive_ptr<Expression>>
parseExpressionReplaceBase(const char* opName,
                           ExpressionContext* const expCtx,
                           BSONElement expr,
                           const VariablesParseState& vps) {

    uassert(51751,
            str::stream() << opName
                          << " requires an object as an argument, found: " << typeName(expr.type()),
            expr.type() == Object);

    intrusive_ptr<Expression> input;
    intrusive_ptr<Expression> find;
    intrusive_ptr<Expression> replacement;
    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "input"_sd) {
            input = Expression::parseOperand(expCtx, elem, vps);
        } else if (field == "find"_sd) {
            find = Expression::parseOperand(expCtx, elem, vps);
        } else if (field == "replacement"_sd) {
            replacement = Expression::parseOperand(expCtx, elem, vps);
        } else {
            uasserted(51750, str::stream() << opName << " found an unknown argument: " << field);
        }
    }

    uassert(51749, str::stream() << opName << " requires 'input' to be specified", input);
    uassert(51748, str::stream() << opName << " requires 'find' to be specified", find);
    uassert(
        51747, str::stream() << opName << " requires 'replacement' to be specified", replacement);

    return {input, find, replacement};
}
}  // namespace

intrusive_ptr<Expression> ExpressionReplaceBase::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kFind] = _children[_kFind]->optimize();
    _children[_kReplacement] = _children[_kReplacement]->optimize();
    return this;
}

/* ------------------------ ExpressionReplaceOne ------------------------ */

REGISTER_STABLE_EXPRESSION(replaceOne, ExpressionReplaceOne::parse);

intrusive_ptr<Expression> ExpressionReplaceOne::parse(ExpressionContext* const expCtx,
                                                      BSONElement expr,
                                                      const VariablesParseState& vps) {
    auto [input, find, replacement] = parseExpressionReplaceBase(opName, expCtx, expr, vps);
    return make_intrusive<ExpressionReplaceOne>(
        expCtx, std::move(input), std::move(find), std::move(replacement));
}

Value ExpressionReplaceOne::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ------------------------ ExpressionReplaceAll ------------------------ */

REGISTER_STABLE_EXPRESSION(replaceAll, ExpressionReplaceAll::parse);

intrusive_ptr<Expression> ExpressionReplaceAll::parse(ExpressionContext* const expCtx,
                                                      BSONElement expr,
                                                      const VariablesParseState& vps) {
    auto [input, find, replacement] = parseExpressionReplaceBase(opName, expCtx, expr, vps);
    return make_intrusive<ExpressionReplaceAll>(
        expCtx, std::move(input), std::move(find), std::move(replacement));
}

Value ExpressionReplaceAll::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

/* ------------------------ ExpressionReverseArray ------------------------ */

Value ExpressionReverseArray::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(reverseArray, ExpressionReverseArray::parse);
const char* ExpressionReverseArray::getOpName() const {
    return "$reverseArray";
}

/* ------------------------ ExpressionSortArray ------------------------ */

namespace {

BSONObj createSortSpecObject(const BSONElement& sortClause) {
    if (sortClause.type() == BSONType::Object) {
        auto status = pattern_cmp::checkSortClause(sortClause.embeddedObject());
        uassert(2942505, status.toString(), status.isOK());

        return sortClause.embeddedObject();
    } else if (sortClause.isNumber()) {
        double orderVal = sortClause.Number();
        uassert(2942506,
                "The $sort element value must be either 1 or -1",
                orderVal == -1 || orderVal == 1);

        return BSON("" << orderVal);
    } else {
        uasserted(2942507,
                  "The $sort is invalid: use 1/-1 to sort the whole element, or {field:1/-1} to "
                  "sort embedded fields");
    }
}

}  // namespace

intrusive_ptr<Expression> ExpressionSortArray::parse(ExpressionContext* const expCtx,
                                                     BSONElement expr,
                                                     const VariablesParseState& vps) {
    uassert(2942500,
            str::stream() << "$sortArray requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == Object);

    boost::intrusive_ptr<Expression> input;
    boost::optional<PatternValueCmp> sortBy;
    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "input") {
            input = parseOperand(expCtx, elem, vps);
        } else if (field == "sortBy") {
            sortBy = PatternValueCmp(createSortSpecObject(elem), elem, expCtx->getCollator());
        } else {
            uasserted(2942501, str::stream() << "$sortArray found an unknown argument: " << field);
        }
    }

    uassert(2942502, "$sortArray requires 'input' to be specified", input);
    uassert(2942503, "$sortArray requires 'sortBy' to be specified", sortBy != boost::none);

    return new ExpressionSortArray(expCtx, std::move(input), *sortBy);
}

Value ExpressionSortArray::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(sortArray, ExpressionSortArray::parse);

const char* ExpressionSortArray::getOpName() const {
    return kName.rawData();
}

intrusive_ptr<Expression> ExpressionSortArray::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    return this;
}

Value ExpressionSortArray::serialize(const SerializationOptions& options) const {
    return Value(Document{{kName,
                           Document{{"input", _children[_kInput]->serialize(options)},
                                    {"sortBy", _sortBy.getOriginalElement()}}}});
}

/* ----------------------- ExpressionSetDifference ---------------------------- */

Value ExpressionSetDifference::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(setDifference, ExpressionSetDifference::parse);
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
    return exec::expression::evaluate(*this, root, variables);
}

/**
 * If there's a constant set in the input, we can construct a hash set for the constant once during
 * optimize() and compare other sets against it, which reduces the runtime to construct the constant
 * sets over and over.
 */
intrusive_ptr<Expression> ExpressionSetEquals::optimize() {
    const size_t n = _children.size();
    const ValueComparator& valueComparator = getExpressionContext()->getValueComparator();

    for (size_t i = 0; i < n; i++) {
        _children[i] = _children[i]->optimize();
        if (ExpressionConstant* ec = dynamic_cast<ExpressionConstant*>(_children[i].get())) {
            const Value nextEntry = ec->getValue();
            uassert(5887502,
                    str::stream() << "All operands of $setEquals must be arrays. " << (i + 1)
                                  << "-th argument is of type: " << typeName(nextEntry.getType()),
                    nextEntry.isArray());

            if (!_cachedConstant) {
                _cachedConstant = std::make_pair(i, valueComparator.makeFlatUnorderedValueSet());
                _cachedConstant->second.insert(nextEntry.getArray().begin(),
                                               nextEntry.getArray().end());
            }
        }
    }

    return this;
}

REGISTER_STABLE_EXPRESSION(setEquals, ExpressionSetEquals::parse);
const char* ExpressionSetEquals::getOpName() const {
    return "$setEquals";
}

/* ----------------------- ExpressionSetIntersection ---------------------------- */

Value ExpressionSetIntersection::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(setIntersection, ExpressionSetIntersection::parse);
const char* ExpressionSetIntersection::getOpName() const {
    return "$setIntersection";
}

/* ----------------------- ExpressionSetIsSubset ---------------------------- */

Value ExpressionSetIsSubset::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

intrusive_ptr<Expression> ExpressionSetIsSubset::optimize() {
    // perform basic optimizations
    intrusive_ptr<Expression> optimized = ExpressionNary::optimize();

    // if ExpressionNary::optimize() created a new value, return it directly
    if (optimized.get() != this)
        return optimized;

    if (ExpressionConstant* ec = dynamic_cast<ExpressionConstant*>(_children[1].get())) {
        const Value rhs = ec->getValue();
        uassert(17311,
                str::stream() << "both operands of $setIsSubset must be arrays. Second "
                              << "argument is of type: " << typeName(rhs.getType()),
                rhs.isArray());

        _cachedRhsSet = exec::expression::arrayToUnorderedSet(
            rhs, getExpressionContext()->getValueComparator());
    }
    return this;
}

REGISTER_STABLE_EXPRESSION(setIsSubset, ExpressionSetIsSubset::parse);
const char* ExpressionSetIsSubset::getOpName() const {
    return "$setIsSubset";
}

/* ----------------------- ExpressionSetUnion ---------------------------- */

Value ExpressionSetUnion::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(setUnion, ExpressionSetUnion::parse);
const char* ExpressionSetUnion::getOpName() const {
    return "$setUnion";
}

/* ----------------------- ExpressionIsArray ---------------------------- */

Value ExpressionIsArray::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(isArray, ExpressionIsArray::parse);
const char* ExpressionIsArray::getOpName() const {
    return "$isArray";
}

/* ----------------------- ExpressionInternalFindAllValuesAtPath --------*/
Value ExpressionInternalFindAllValuesAtPath::evaluate(const Document& root,
                                                      Variables* variables) const {

    auto fieldPath = getFieldPath();
    BSONElementSet elts(getExpressionContext()->getCollator());
    auto bsonRoot = root.toBson();
    dotted_path_support::extractAllElementsAlongPath(bsonRoot, fieldPath.fullPath(), elts);
    std::vector<Value> outputVals;
    for (BSONElementSet::iterator it = elts.begin(); it != elts.end(); ++it) {
        BSONElement elt = *it;
        outputVals.push_back(Value(elt));
    }

    return Value(std::move(outputVals));
}
// This expression is not part of the stable API, but can always be used. It is
// an internal expression used only for distinct.
REGISTER_STABLE_EXPRESSION(_internalFindAllValuesAtPath,
                           ExpressionInternalFindAllValuesAtPath::parse);

/* ----------------------- ExpressionSlice ---------------------------- */

Value ExpressionSlice::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(slice, ExpressionSlice::parse);
const char* ExpressionSlice::getOpName() const {
    return "$slice";
}

/* ----------------------- ExpressionSigmoid ----------------------- */

/**
 * A $sigmoid expression gets desugared to the following:
 * {
 *     $divide: [1,
 *         { $add: [
 *             1,
 *             { $exp: {$multiply: [-1, <input>]} }
 *         ] }
 *     ]
 * }
 */
intrusive_ptr<Expression> ExpressionSigmoid::parseExpressionSigmoid(
    ExpressionContext* const expCtx, BSONElement expr, const VariablesParseState& vps) {
    // TODO SERVER-92973: Improve error handling so that any field or expression that resolves to a
    // non-numeric input is handled by $sigmoid instead of the desugared $multiply.

    // Check that the input to $sigmoid is not an array or a string.
    BSONType type = expr.type();
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$sigmoid only supports numeric types, not " << typeName(type),
            (type != String ? true : expr.valueStringData().starts_with('$')) && type != Array);

    auto inputExpression = Expression::parseOperand(expCtx, expr, vps);

    // Built the multiply expression: {$multiply: [-1, <input>]}.
    std::vector<boost::intrusive_ptr<Expression>> multiplyChildren = {
        make_intrusive<ExpressionConstant>(expCtx, Value(-1)), std::move(inputExpression)};
    auto multiplyExpression =
        make_intrusive<ExpressionMultiply>(expCtx, std::move(multiplyChildren));

    // Built the exponent expression: { $exp: {$multiply: [-1, <input>]} }.
    std::vector<boost::intrusive_ptr<Expression>> expChildren = {std::move(multiplyExpression)};
    auto expExpression = make_intrusive<ExpressionExp>(expCtx, std::move(expChildren));

    // Built the addition expression: { $add: [1, {$exp: {$multiply: [-1, <input>]} }.
    std::vector<boost::intrusive_ptr<Expression>> addChildren = {
        make_intrusive<ExpressionConstant>(expCtx, Value(1)), std::move(expExpression)};
    auto addExpression = make_intrusive<ExpressionAdd>(expCtx, std::move(addChildren));

    // Build and return the divide expression: { $divide: [1, { $add: [1, { $exp: {$multiply: [-1,
    // <input>]} }] }] }.
    std::vector<boost::intrusive_ptr<Expression>> divideChildren = {
        make_intrusive<ExpressionConstant>(expCtx, Value(1)), std::move(addExpression)};
    return make_intrusive<ExpressionDivide>(expCtx, std::move(divideChildren));
}

REGISTER_EXPRESSION_WITH_FEATURE_FLAG(sigmoid,
                                      ExpressionSigmoid::parseExpressionSigmoid,
                                      AllowedWithApiStrict::kNeverInVersion1,
                                      AllowedWithClientType::kAny,
                                      feature_flags::gFeatureFlagSearchHybridScoringPrerequisites);

/* ----------------------- ExpressionSize ---------------------------- */

Value ExpressionSize::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(size, ExpressionSize::parse);
const char* ExpressionSize::getOpName() const {
    return "$size";
}

/* ----------------------- ExpressionSplit --------------------------- */

Value ExpressionSplit::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(split, ExpressionSplit::parse);
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

REGISTER_STABLE_EXPRESSION(sqrt, ExpressionSqrt::parse);
const char* ExpressionSqrt::getOpName() const {
    return "$sqrt";
}

/* ----------------------- ExpressionStrcasecmp ---------------------------- */

Value ExpressionStrcasecmp::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(strcasecmp, ExpressionStrcasecmp::parse);
const char* ExpressionStrcasecmp::getOpName() const {
    return "$strcasecmp";
}

/* ----------------------- ExpressionSubstrBytes ---------------------------- */

Value ExpressionSubstrBytes::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

// $substr is deprecated in favor of $substrBytes, but for now will just parse into a $substrBytes.
REGISTER_STABLE_EXPRESSION(substrBytes, ExpressionSubstrBytes::parse);
REGISTER_STABLE_EXPRESSION(substr, ExpressionSubstrBytes::parse);
const char* ExpressionSubstrBytes::getOpName() const {
    return "$substrBytes";
}

/* ----------------------- ExpressionSubstrCP ---------------------------- */

Value ExpressionSubstrCP::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(substrCP, ExpressionSubstrCP::parse);
const char* ExpressionSubstrCP::getOpName() const {
    return "$substrCP";
}

/* ----------------------- ExpressionStrLenBytes ------------------------- */

Value ExpressionStrLenBytes::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(strLenBytes, ExpressionStrLenBytes::parse);
const char* ExpressionStrLenBytes::getOpName() const {
    return "$strLenBytes";
}

/* -------------------------- ExpressionBinarySize ------------------------------ */

Value ExpressionBinarySize::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(binarySize, ExpressionBinarySize::parse);

const char* ExpressionBinarySize::getOpName() const {
    return "$binarySize";
}

/* ----------------------- ExpressionStrLenCP ------------------------- */

Value ExpressionStrLenCP::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(strLenCP, ExpressionStrLenCP::parse);
const char* ExpressionStrLenCP::getOpName() const {
    return "$strLenCP";
}

/* ----------------------- ExpressionSubtract ---------------------------- */

Value ExpressionSubtract::evaluate(const Document& root, Variables* variables) const {
    return uassertStatusOK(
        apply(_children[0]->evaluate(root, variables), _children[1]->evaluate(root, variables)));
}

StatusWith<Value> ExpressionSubtract::apply(Value lhs, Value rhs) {
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
        if (overflow::sub(lhs.coerceToLong(), rhs.coerceToLong(), &result)) {
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
        BSONType rhsType = rhs.getType();
        switch (rhsType) {
            case Date:
                return Value(durationCount<Milliseconds>(lhs.getDate() - rhs.getDate()));
            case NumberInt:
            case NumberLong: {
                long long longDiff = lhs.getDate().toMillisSinceEpoch();
                if (overflow::sub(longDiff, rhs.coerceToLong(), &longDiff)) {
                    return Status(ErrorCodes::Overflow, str::stream() << "date overflow");
                }
                return Value(Date_t::fromMillisSinceEpoch(longDiff));
            }
            case NumberDouble: {
                using limits = std::numeric_limits<long long>;
                long long longDiff = lhs.getDate().toMillisSinceEpoch();
                double doubleRhs = rhs.coerceToDouble();
                // check the doubleRhs should not exceed int64 limit and result will not overflow
                if (doubleRhs >= static_cast<double>(limits::min()) &&
                    doubleRhs < static_cast<double>(limits::max()) &&
                    !overflow::sub(longDiff, llround(doubleRhs), &longDiff)) {
                    return Value(Date_t::fromMillisSinceEpoch(longDiff));
                }
                return Status(ErrorCodes::Overflow, str::stream() << "date overflow");
            }
            case NumberDecimal: {
                long long longDiff = lhs.getDate().toMillisSinceEpoch();
                Decimal128 decimalRhs = rhs.coerceToDecimal();
                std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
                std::int64_t longRhs = decimalRhs.toLong(&signalingFlags);
                if (signalingFlags != Decimal128::SignalingFlag::kNoFlag ||
                    overflow::sub(longDiff, longRhs, &longDiff)) {
                    return Status(ErrorCodes::Overflow, str::stream() << "date overflow");
                }
                return Value(Date_t::fromMillisSinceEpoch(longDiff));
            }
            default:
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "can't $subtract " << typeName(rhs.getType()) << " from Date");
        }
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "can't $subtract " << typeName(rhs.getType()) << " from "
                                    << typeName(lhs.getType()));
    }
}

REGISTER_STABLE_EXPRESSION(subtract, ExpressionSubtract::parse);
const char* ExpressionSubtract::getOpName() const {
    return "$subtract";
}

monotonic::State ExpressionSubtract::getMonotonicState(const FieldPath& sortedFieldPath) const {
    // 1. Get monotonic states of the both children.
    // 2. Apply monotonic::opposite to the state of the second child, because it is negated.
    // 3. Combine children. Function monotonic::combine correctly handles all the cases where, for
    // example, argumemnts are both monotonic, but in the opposite directions.
    return monotonic::combine(
        getChildren()[0]->getMonotonicState(sortedFieldPath),
        monotonic::opposite(getChildren()[1]->getMonotonicState(sortedFieldPath)));
}

/* ------------------------- ExpressionSwitch ------------------------------ */

REGISTER_STABLE_EXPRESSION(switch, ExpressionSwitch::parse);

Value ExpressionSwitch::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionSwitch::parse(ExpressionContext* const expCtx,
                                                         BSONElement expr,
                                                         const VariablesParseState& vps) {
    uassert(40060,
            str::stream() << "$switch requires an object as an argument, found: "
                          << typeName(expr.type()),
            expr.type() == BSONType::Object);

    boost::intrusive_ptr<Expression> expDefault;
    std::vector<boost::intrusive_ptr<Expression>> children;
    for (auto&& elem : expr.Obj()) {
        auto field = elem.fieldNameStringData();

        if (field == "branches") {
            // Parse each branch separately.
            uassert(40061,
                    str::stream() << "$switch expected an array for 'branches', found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Array);

            for (auto&& branch : elem.Array()) {
                uassert(40062,
                        str::stream() << "$switch expected each branch to be an object, found: "
                                      << typeName(branch.type()),
                        branch.type() == BSONType::Object);

                boost::intrusive_ptr<Expression> switchCase, switchThen;

                for (auto&& branchElement : branch.Obj()) {
                    auto branchField = branchElement.fieldNameStringData();

                    if (branchField == "case") {
                        switchCase = parseOperand(expCtx, branchElement, vps);
                    } else if (branchField == "then") {
                        switchThen = parseOperand(expCtx, branchElement, vps);
                    } else {
                        uasserted(40063,
                                  str::stream() << "$switch found an unknown argument to a branch: "
                                                << branchField);
                    }
                }

                uassert(40064, "$switch requires each branch have a 'case' expression", switchCase);
                uassert(
                    40065, "$switch requires each branch have a 'then' expression.", switchThen);

                children.push_back(std::move(switchCase));
                children.push_back(std::move(switchThen));
            }
        } else if (field == "default") {
            // Optional, arbitrary expression.
            expDefault = parseOperand(expCtx, elem, vps);
        } else {
            uasserted(40067, str::stream() << "$switch found an unknown argument: " << field);
        }
    }

    // The the 'default' expression is always the final child. If no 'default' expression is
    // provided, then the final child is nullptr.
    children.push_back(std::move(expDefault));

    return new ExpressionSwitch(expCtx, std::move(children));
}

void ExpressionSwitch::deleteBranch(int i) {
    invariant(i >= 0);
    invariant(i < numBranches());
    // Delete the two elements corresponding to this branch at positions 2i and 2i + 1.
    _children.erase(std::next(_children.begin(), i * 2), std::next(_children.begin(), i * 2 + 2));
}

boost::intrusive_ptr<Expression> ExpressionSwitch::optimize() {
    if (defaultExpr()) {
        _children.back() = _children.back()->optimize();
    }

    bool trueConst = false;

    int i = 0;
    while (!trueConst && i < numBranches()) {
        boost::intrusive_ptr<Expression>& caseExpr = _children[i * 2];
        boost::intrusive_ptr<Expression>& thenExpr = _children[i * 2 + 1];
        caseExpr = caseExpr->optimize();

        if (auto* val = dynamic_cast<ExpressionConstant*>(caseExpr.get())) {
            if (!val->getValue().coerceToBool()) {
                // Case is constant and evaluates to false, so it is removed.
                deleteBranch(i);
            } else {
                // Case optimized to a constant true value. Set the optimized version of the
                // corresponding 'then' expression as the new 'default'. Break out of the loop and
                // fall through to the logic to remove this and all subsequent branches.
                trueConst = true;
                _children.back() = thenExpr->optimize();
                break;
            }
        } else {
            // Since case is not removed from the switch, its then is now optimized.
            thenExpr = thenExpr->optimize();
            ++i;
        }
    }

    // Erasing the rest of the cases because found a default true value.
    if (trueConst) {
        while (i < numBranches()) {
            deleteBranch(i);
        }
    }

    // If there are no cases, make the switch its default.
    if (numBranches() == 0) {
        uassert(40069,
                "Cannot execute a switch statement where all the cases evaluate to false "
                "without a default",
                defaultExpr());
        return _children.back();
    }

    return this;
}

Value ExpressionSwitch::serialize(const SerializationOptions& options) const {
    std::vector<Value> serializedBranches;
    serializedBranches.reserve(numBranches());

    for (int i = 0; i < numBranches(); ++i) {
        auto [caseExpr, thenExpr] = getBranch(i);
        serializedBranches.push_back(Value(Document{{"case", caseExpr->serialize(options)},
                                                    {"then", thenExpr->serialize(options)}}));
    }

    if (defaultExpr()) {
        return Value(Document{{"$switch",
                               Document{{"branches", Value(std::move(serializedBranches))},
                                        {"default", defaultExpr()->serialize(options)}}}});
    }

    return Value(
        Document{{"$switch", Document{{"branches", Value(std::move(serializedBranches))}}}});
}

/* ------------------------- ExpressionToLower ----------------------------- */

Value ExpressionToLower::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(toLower, ExpressionToLower::parse);
const char* ExpressionToLower::getOpName() const {
    return "$toLower";
}

/* ------------------------- ExpressionToUpper -------------------------- */

Value ExpressionToUpper::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(toUpper, ExpressionToUpper::parse);
const char* ExpressionToUpper::getOpName() const {
    return "$toUpper";
}

/* -------------------------- ExpressionTrim ------------------------------ */

REGISTER_STABLE_EXPRESSION(trim, ExpressionTrim::parse);
REGISTER_STABLE_EXPRESSION(ltrim, ExpressionTrim::parse);
REGISTER_STABLE_EXPRESSION(rtrim, ExpressionTrim::parse);

intrusive_ptr<Expression> ExpressionTrim::parse(ExpressionContext* const expCtx,
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

Value ExpressionTrim::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionTrim::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    if (_children[_kCharacters]) {
        _children[_kCharacters] = _children[_kCharacters]->optimize();
    }
    if (ExpressionConstant::allNullOrConstant({_children[_kInput], _children[_kCharacters]})) {
        return ExpressionConstant::create(
            getExpressionContext(),
            this->evaluate(Document(), &(getExpressionContext()->variables)));
    }
    return this;
}

Value ExpressionTrim::serialize(const SerializationOptions& options) const {
    return Value(
        Document{{_name,
                  Document{{"input", _children[_kInput]->serialize(options)},
                           {"chars",
                            _children[_kCharacters] ? _children[_kCharacters]->serialize(options)
                                                    : Value()}}}});
}

/* ------------------------- ExpressionRound and ExpressionTrunc -------------------------- */

void assertFlagsValid(uint32_t flags,
                      const std::string& opName,
                      long long numericValue,
                      long long precisionValue) {
    uassert(51080,
            str::stream() << "invalid conversion from Decimal128 result in " << opName
                          << " resulting from arguments: [" << numericValue << ", "
                          << precisionValue << "]",
            !Decimal128::hasFlag(flags, Decimal128::kInvalid));
}

static Value evaluateRoundOrTrunc(const Document& root,
                                  const std::vector<boost::intrusive_ptr<Expression>>& children,
                                  const std::string& opName,
                                  Decimal128::RoundingMode roundingMode,
                                  double (*doubleOp)(double),
                                  Variables* variables) {
    constexpr auto maxPrecision = 100LL;
    constexpr auto minPrecision = -20LL;
    auto numericArg = Value(children[0]->evaluate(root, variables));
    if (numericArg.nullish()) {
        return Value(BSONNULL);
    }
    uassert(51081,
            str::stream() << opName << " only supports numeric types, not "
                          << typeName(numericArg.getType()),
            numericArg.numeric());

    long long precisionValue = 0;
    if (children.size() > 1) {
        auto precisionArg = Value(children[1]->evaluate(root, variables));
        if (precisionArg.nullish()) {
            return Value(BSONNULL);
        }
        precisionValue = precisionArg.coerceToLong();
        uassert(51082,
                str::stream() << "precision argument to  " << opName << " must be a integral value",
                precisionArg.integral());
        uassert(51083,
                str::stream() << "cannot apply " << opName << " with precision value "
                              << precisionValue << " value must be in [-20, 100]",
                minPrecision <= precisionValue && precisionValue <= maxPrecision);
    }

    // Construct 10^-precisionValue, which will be used as the quantize reference.
    auto quantum = Decimal128(0LL, Decimal128::kExponentBias - precisionValue, 0LL, 1LL);
    switch (numericArg.getType()) {
        case BSONType::NumberDecimal: {
            if (numericArg.getDecimal().isInfinite()) {
                return numericArg;
            }
            auto out = numericArg.getDecimal().quantize(quantum, roundingMode);
            return Value(out);
        }
        case BSONType::NumberDouble: {
            auto dec = Decimal128(numericArg.getDouble(), Decimal128::kRoundTo34Digits);
            if (dec.isInfinite()) {
                return numericArg;
            }
            auto out = dec.quantize(quantum, roundingMode);
            return Value(out.toDouble());
        }
        case BSONType::NumberInt:
        case BSONType::NumberLong: {
            if (precisionValue >= 0) {
                return numericArg;
            }
            auto numericArgll = numericArg.getLong();
            auto out =
                Decimal128(static_cast<int64_t>(numericArgll)).quantize(quantum, roundingMode);
            uint32_t flags = 0;
            auto outll = out.toLong(&flags);
            assertFlagsValid(flags, opName, numericArgll, precisionValue);
            if (numericArg.getType() == BSONType::NumberLong ||
                outll > std::numeric_limits<int>::max()) {
                // Even if the original was an int to begin with - it has to be a long now.
                return Value(static_cast<long long>(outll));
            }
            return Value(static_cast<int>(outll));
        }
        default:
            MONGO_UNREACHABLE;
    }
}

Value ExpressionRound::evaluate(const Document& root, Variables* variables) const {
    return evaluateRoundOrTrunc(
        root, _children, getOpName(), Decimal128::kRoundTiesToEven, &std::round, variables);
}

REGISTER_STABLE_EXPRESSION(round, ExpressionRound::parse);
const char* ExpressionRound::getOpName() const {
    return "$round";
}

Value ExpressionTrunc::evaluate(const Document& root, Variables* variables) const {
    return evaluateRoundOrTrunc(
        root, _children, getOpName(), Decimal128::kRoundTowardZero, &std::trunc, variables);
}

REGISTER_STABLE_EXPRESSION(trunc, ExpressionTrunc::parse);
const char* ExpressionTrunc::getOpName() const {
    return "$trunc";
}

/* ------------------------- ExpressionType ----------------------------- */

Value ExpressionType::evaluate(const Document& root, Variables* variables) const {
    Value val(_children[0]->evaluate(root, variables));
    return Value(StringData(typeName(val.getType())));
}

REGISTER_STABLE_EXPRESSION(type, ExpressionType::parse);
const char* ExpressionType::getOpName() const {
    return "$type";
}

/* ------------------------ ExpressionIsNumber --------------------------- */

Value ExpressionIsNumber::evaluate(const Document& root, Variables* variables) const {
    Value val(_children[0]->evaluate(root, variables));
    return Value(val.numeric());
}

REGISTER_STABLE_EXPRESSION(isNumber, ExpressionIsNumber::parse);

const char* ExpressionIsNumber::getOpName() const {
    return "$isNumber";
}

/* -------------------------- ExpressionZip ------------------------------ */

REGISTER_STABLE_EXPRESSION(zip, ExpressionZip::parse);
intrusive_ptr<Expression> ExpressionZip::parse(ExpressionContext* const expCtx,
                                               BSONElement expr,
                                               const VariablesParseState& vps) {
    uassert(34460,
            str::stream() << "$zip only supports an object as an argument, found "
                          << typeName(expr.type()),
            expr.type() == Object);

    auto useLongestLength = false;
    std::vector<boost::intrusive_ptr<Expression>> children;
    // We need to ensure defaults appear after inputs so we build them seperately and then
    // concatenate them.
    std::vector<boost::intrusive_ptr<Expression>> tempDefaultChildren;

    for (auto&& elem : expr.Obj()) {
        const auto field = elem.fieldNameStringData();
        if (field == "inputs") {
            uassert(34461,
                    str::stream() << "inputs must be an array of expressions, found "
                                  << typeName(elem.type()),
                    elem.type() == Array);
            for (auto&& subExpr : elem.Array()) {
                children.push_back(parseOperand(expCtx, subExpr, vps));
            }
        } else if (field == "defaults") {
            uassert(34462,
                    str::stream() << "defaults must be an array of expressions, found "
                                  << typeName(elem.type()),
                    elem.type() == Array);
            for (auto&& subExpr : elem.Array()) {
                tempDefaultChildren.push_back(parseOperand(expCtx, subExpr, vps));
            }
        } else if (field == "useLongestLength") {
            uassert(34463,
                    str::stream() << "useLongestLength must be a bool, found "
                                  << typeName(expr.type()),
                    elem.type() == Bool);
            useLongestLength = elem.Bool();
        } else {
            uasserted(34464,
                      str::stream() << "$zip found an unknown argument: " << elem.fieldName());
        }
    }

    auto numInputs = children.size();
    std::move(tempDefaultChildren.begin(), tempDefaultChildren.end(), std::back_inserter(children));

    std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> inputs;
    std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> defaults;
    for (auto&& child : children) {
        if (numInputs == 0) {
            defaults.push_back(child);
        } else {
            inputs.push_back(child);
            numInputs--;
        }
    }

    uassert(34465, "$zip requires at least one input array", !inputs.empty());
    uassert(34466,
            "cannot specify defaults unless useLongestLength is true",
            (useLongestLength || defaults.empty()));
    uassert(34467,
            "defaults and inputs must have the same length",
            (defaults.empty() || defaults.size() == inputs.size()));

    return new ExpressionZip(
        expCtx, useLongestLength, std::move(children), std::move(inputs), std::move(defaults));
}

Value ExpressionZip::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionZip::optimize() {
    for (auto&& input : _inputs)
        input.get() = input.get()->optimize();
    for (auto&& zipDefault : _defaults)
        zipDefault.get() = zipDefault.get()->optimize();
    return this;
}

Value ExpressionZip::serialize(const SerializationOptions& options) const {
    vector<Value> serializedInput;
    vector<Value> serializedDefaults;
    Value serializedUseLongestLength = Value(_useLongestLength);

    for (auto&& expr : _inputs) {
        serializedInput.push_back(expr.get()->serialize(options));
    }

    for (auto&& expr : _defaults) {
        serializedDefaults.push_back(expr.get()->serialize(options));
    }

    return Value(DOC("$zip" << DOC("inputs" << Value(serializedInput) << "defaults"
                                            << Value(serializedDefaults) << "useLongestLength"
                                            << serializedUseLongestLength)));
}

/* -------------------------- ExpressionConvert ------------------------------ */

namespace {

/**
 * $convert supports a big grab bag of conversions, so ConversionTable maintains a collection of
 * conversion functions, as well as a table to organize them by inputType and targetType.
 */
class ConversionTable {
public:
    // Some conversion functions require extra arguments like format and subtype. However,
    // ConversionTable is expected to return a regular 'ConversionFunc'. Functions with extra
    // arguments are curried in 'makeConversionFunc' to accept just two arguments,
    // the expression context and an input value. The extra arguments are expected to be movable.
    template <typename... ExtraArgs>
    using ConversionFuncWithExtraArgs =
        std::function<Value(ExpressionContext* const, Value, ExtraArgs...)>;

    using FormatArg = BinDataFormat;
    using SubtypeArg = Value;
    using ByteOrderArg = ConvertByteOrderType;

    using ConversionFunc = ConversionFuncWithExtraArgs<>;
    using ConversionFuncWithFormat = ConversionFuncWithExtraArgs<FormatArg>;
    using ConversionFuncWithSubtype = ConversionFuncWithExtraArgs<SubtypeArg>;
    using ConversionFuncWithFormatAndSubtype = ConversionFuncWithExtraArgs<FormatArg, SubtypeArg>;
    using ConversionFuncWithByteOrder = ConversionFuncWithExtraArgs<ByteOrderArg>;
    using ConversionFuncWithByteOrderAndSubtype =
        ConversionFuncWithExtraArgs<ByteOrderArg, SubtypeArg>;

    using AnyConversionFunc = std::variant<std::monostate,
                                           ConversionFunc,
                                           ConversionFuncWithFormat,
                                           ConversionFuncWithSubtype,
                                           ConversionFuncWithFormatAndSubtype,
                                           ConversionFuncWithByteOrder,
                                           ConversionFuncWithByteOrderAndSubtype>;

    ConversionTable() {
        //
        // Conversions from NumberDouble
        //
        table[BSONType::NumberDouble][BSONType::NumberDouble] = &performIdentityConversion;
        table[BSONType::NumberDouble][BSONType::String] = &performFormatDouble;
        table[BSONType::NumberDouble][BSONType::Bool] = [](ExpressionContext* const expCtx,
                                                           Value inputValue) {
            return Value(inputValue.coerceToBool());
        };
        table[BSONType::NumberDouble][BSONType::Date] = &performCastNumberToDate;
        table[BSONType::NumberDouble][BSONType::NumberInt] = &performCastDoubleToInt;
        table[BSONType::NumberDouble][BSONType::NumberLong] = &performCastDoubleToLong;
        table[BSONType::NumberDouble][BSONType::NumberDecimal] = [](ExpressionContext* const expCtx,
                                                                    Value inputValue) {
            return Value(inputValue.coerceToDecimal());
        };
        table[BSONType::NumberDouble][BSONType::BinData] = &performConvertDoubleToBinData;

        //
        // Conversions from String
        //
        table[BSONType::String][BSONType::NumberDouble] = &parseStringToNumber<double, 0>;
        table[BSONType::String][BSONType::String] = &performIdentityConversion;
        table[BSONType::String][BSONType::jstOID] = &parseStringToOID;
        table[BSONType::String][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::String][BSONType::Date] = [](ExpressionContext* const expCtx,
                                                     Value inputValue) {
            return Value(expCtx->getTimeZoneDatabase()->fromString(
                inputValue.getStringData(), mongo::TimeZoneDatabase::utcZone()));
        };
        table[BSONType::String][BSONType::NumberInt] = &parseStringToNumber<int, 10>;
        table[BSONType::String][BSONType::NumberLong] = &parseStringToNumber<long long, 10>;
        table[BSONType::String][BSONType::NumberDecimal] = &parseStringToNumber<Decimal128, 0>;
        table[BSONType::String][BSONType::BinData] = &parseStringToBinData;

        //
        // Conversions from BinData
        //
        table[BSONType::BinData][BSONType::BinData] = &performConvertBinDataToBinData;
        table[BSONType::BinData][BSONType::String] = &performConvertBinDataToString;
        table[BSONType::BinData][BSONType::NumberInt] = &performConvertBinDataToInt;
        table[BSONType::BinData][BSONType::NumberLong] = &performConvertBinDataToLong;
        table[BSONType::BinData][BSONType::NumberDouble] = &performConvertBinDataToDouble;

        //
        // Conversions from jstOID
        //
        table[BSONType::jstOID][BSONType::String] = [](ExpressionContext* const expCtx,
                                                       Value inputValue) {
            return Value(inputValue.getOid().toString());
        };
        table[BSONType::jstOID][BSONType::jstOID] = &performIdentityConversion;
        table[BSONType::jstOID][BSONType::Bool] = &performConvertToTrue;
        table[BSONType::jstOID][BSONType::Date] = [](ExpressionContext* const expCtx,
                                                     Value inputValue) {
            return Value(inputValue.getOid().asDateT());
        };

        //
        // Conversions from Bool
        //
        table[BSONType::Bool][BSONType::NumberDouble] = [](ExpressionContext* const expCtx,
                                                           Value inputValue) {
            return inputValue.getBool() ? Value(1.0) : Value(0.0);
        };
        table[BSONType::Bool][BSONType::String] = [](ExpressionContext* const expCtx,
                                                     Value inputValue) {
            return inputValue.getBool() ? Value("true"_sd) : Value("false"_sd);
        };
        table[BSONType::Bool][BSONType::Bool] = &performIdentityConversion;
        table[BSONType::Bool][BSONType::NumberInt] = [](ExpressionContext* const expCtx,
                                                        Value inputValue) {
            return inputValue.getBool() ? Value(int{1}) : Value(int{0});
        };
        table[BSONType::Bool][BSONType::NumberLong] = [](ExpressionContext* const expCtx,
                                                         Value inputValue) {
            return inputValue.getBool() ? Value(1LL) : Value(0LL);
        };
        table[BSONType::Bool][BSONType::NumberDecimal] = [](ExpressionContext* const expCtx,
                                                            Value inputValue) {
            return inputValue.getBool() ? Value(Decimal128(1)) : Value(Decimal128(0));
        };

        //
        // Conversions from Date
        //
        table[BSONType::Date][BSONType::NumberDouble] = [](ExpressionContext* const expCtx,
                                                           Value inputValue) {
            return Value(static_cast<double>(inputValue.getDate().toMillisSinceEpoch()));
        };
        table[BSONType::Date][BSONType::String] = [](ExpressionContext* const expCtx,
                                                     Value inputValue) {
            auto dateString = uassertStatusOK(
                TimeZoneDatabase::utcZone().formatDate(kIsoFormatStringZ, inputValue.getDate()));
            return Value(dateString);
        };
        table[BSONType::Date][BSONType::Bool] = [](ExpressionContext* const expCtx,
                                                   Value inputValue) {
            return Value(inputValue.coerceToBool());
        };
        table[BSONType::Date][BSONType::Date] = &performIdentityConversion;
        table[BSONType::Date][BSONType::NumberLong] = [](ExpressionContext* const expCtx,
                                                         Value inputValue) {
            return Value(inputValue.getDate().toMillisSinceEpoch());
        };
        table[BSONType::Date][BSONType::NumberDecimal] = [](ExpressionContext* const expCtx,
                                                            Value inputValue) {
            return Value(
                Decimal128(static_cast<int64_t>(inputValue.getDate().toMillisSinceEpoch())));
        };

        //
        // Conversions from bsonTimestamp
        //
        table[BSONType::bsonTimestamp][BSONType::Date] = [](ExpressionContext* const expCtx,
                                                            Value inputValue) {
            return Value(inputValue.coerceToDate());
        };

        //
        // Conversions from NumberInt
        //
        table[BSONType::NumberInt][BSONType::NumberDouble] = [](ExpressionContext* const expCtx,
                                                                Value inputValue) {
            return Value(inputValue.coerceToDouble());
        };
        table[BSONType::NumberInt][BSONType::String] = [](ExpressionContext* const expCtx,
                                                          Value inputValue) {
            return Value(static_cast<std::string>(str::stream() << inputValue.getInt()));
        };
        table[BSONType::NumberInt][BSONType::Bool] = [](ExpressionContext* const expCtx,
                                                        Value inputValue) {
            return Value(inputValue.coerceToBool());
        };
        table[BSONType::NumberInt][BSONType::NumberInt] = &performIdentityConversion;
        table[BSONType::NumberInt][BSONType::NumberLong] = [](ExpressionContext* const expCtx,
                                                              Value inputValue) {
            return Value(static_cast<long long>(inputValue.getInt()));
        };
        table[BSONType::NumberInt][BSONType::NumberDecimal] = [](ExpressionContext* const expCtx,
                                                                 Value inputValue) {
            return Value(inputValue.coerceToDecimal());
        };
        table[BSONType::NumberInt][BSONType::BinData] = &performConvertIntToBinData;

        //
        // Conversions from NumberLong
        //
        table[BSONType::NumberLong][BSONType::NumberDouble] = [](ExpressionContext* const expCtx,
                                                                 Value inputValue) {
            return Value(inputValue.coerceToDouble());
        };
        table[BSONType::NumberLong][BSONType::String] = [](ExpressionContext* const expCtx,
                                                           Value inputValue) {
            return Value(static_cast<std::string>(str::stream() << inputValue.getLong()));
        };
        table[BSONType::NumberLong][BSONType::Bool] = [](ExpressionContext* const expCtx,
                                                         Value inputValue) {
            return Value(inputValue.coerceToBool());
        };
        table[BSONType::NumberLong][BSONType::Date] = &performCastNumberToDate;
        table[BSONType::NumberLong][BSONType::NumberInt] = &performCastLongToInt;
        table[BSONType::NumberLong][BSONType::NumberLong] = &performIdentityConversion;
        table[BSONType::NumberLong][BSONType::NumberDecimal] = [](ExpressionContext* const expCtx,
                                                                  Value inputValue) {
            return Value(inputValue.coerceToDecimal());
        };
        table[BSONType::NumberLong][BSONType::BinData] = &performConvertLongToBinData;

        //
        // Conversions from NumberDecimal
        //
        table[BSONType::NumberDecimal][BSONType::NumberDouble] = &performCastDecimalToDouble;
        table[BSONType::NumberDecimal][BSONType::String] = [](ExpressionContext* const expCtx,
                                                              Value inputValue) {
            return Value(inputValue.getDecimal().toString());
        };
        table[BSONType::NumberDecimal][BSONType::Bool] = [](ExpressionContext* const expCtx,
                                                            Value inputValue) {
            return Value(inputValue.coerceToBool());
        };
        table[BSONType::NumberDecimal][BSONType::Date] = &performCastNumberToDate;
        table[BSONType::NumberDecimal][BSONType::NumberInt] = [](ExpressionContext* const expCtx,
                                                                 Value inputValue) {
            return performCastDecimalToInt(BSONType::NumberInt, inputValue);
        };
        table[BSONType::NumberDecimal][BSONType::NumberLong] = [](ExpressionContext* const expCtx,
                                                                  Value inputValue) {
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

    ConversionFunc findConversionFunc(BSONType inputType,
                                      BSONType targetType,
                                      boost::optional<FormatArg> format,
                                      SubtypeArg subtype,
                                      boost::optional<ByteOrderArg> byteOrder) const {
        AnyConversionFunc foundFunction;

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

        return makeConversionFunc(foundFunction,
                                  inputType,
                                  targetType,
                                  std::move(format),
                                  std::move(subtype),
                                  std::move(byteOrder));
    }

private:
    AnyConversionFunc table[JSTypeMax + 1][JSTypeMax + 1];

    ConversionFunc makeConversionFunc(AnyConversionFunc foundFunction,
                                      BSONType inputType,
                                      BSONType targetType,
                                      boost::optional<FormatArg> format,
                                      SubtypeArg subtype,
                                      boost::optional<ByteOrderArg> byteOrder) const {
        const auto checkFormat = [&] {
            uassert(4341115,
                    str::stream() << "Format must be speficied when converting from '"
                                  << typeName(inputType) << "' to '" << typeName(targetType) << "'",
                    format);
        };

        const auto checkSubtype = [&] {
            // Subtype has a default value so we should never hit this.
            tassert(4341103,
                    str::stream() << "Can't convert to " << typeName(targetType)
                                  << " without knowing subtype",
                    !subtype.missing());
        };

        return visit(OverloadedVisitor{
                         [](ConversionFunc conversionFunc) {
                             tassert(4341109, "Conversion function can't be null", conversionFunc);
                             return conversionFunc;
                         },
                         [&](ConversionFuncWithFormat conversionFunc) {
                             checkFormat();
                             return makeConvertWithExtraArgs(conversionFunc, std::move(*format));
                         },
                         [&](ConversionFuncWithSubtype conversionFunc) {
                             checkSubtype();
                             return makeConvertWithExtraArgs(conversionFunc, std::move(subtype));
                         },
                         [&](ConversionFuncWithFormatAndSubtype conversionFunc) {
                             checkFormat();
                             checkSubtype();
                             return makeConvertWithExtraArgs(
                                 conversionFunc, std::move(*format), std::move(subtype));
                         },
                         [&](ConversionFuncWithByteOrder conversionFunc) {
                             return makeConvertWithExtraArgs(
                                 conversionFunc,
                                 std::move(byteOrder ? *byteOrder : ConvertByteOrderType::little));
                         },
                         [&](ConversionFuncWithByteOrderAndSubtype conversionFunc) {
                             return makeConvertWithExtraArgs(
                                 conversionFunc,
                                 std::move(byteOrder ? *byteOrder : ConvertByteOrderType::little),
                                 std::move(subtype));
                         },
                         [&](std::monostate) -> ConversionFunc {
                             uasserted(ErrorCodes::ConversionFailure,
                                       str::stream()
                                           << "Unsupported conversion from " << typeName(inputType)
                                           << " to " << typeName(targetType)
                                           << " in $convert with no onError value");
                         }},
                     foundFunction);
    }

    static void validateDoubleValueIsFinite(double inputDouble) {
        uassert(ErrorCodes::ConversionFailure,
                "Attempt to convert NaN value to integer type in $convert with no onError value",
                !std::isnan(inputDouble));
        uassert(
            ErrorCodes::ConversionFailure,
            "Attempt to convert infinity value to integer type in $convert with no onError value",
            std::isfinite(inputDouble));
    }

    static Value performCastDoubleToInt(ExpressionContext* const expCtx, Value inputValue) {
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

    static Value performCastDoubleToLong(ExpressionContext* const expCtx, Value inputValue) {
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

    static Value performCastDecimalToDouble(ExpressionContext* const expCtx, Value inputValue) {
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

    static Value performCastLongToInt(ExpressionContext* const expCtx, Value inputValue) {
        long long longValue = inputValue.getLong();

        uassert(ErrorCodes::ConversionFailure,
                str::stream()
                    << "Conversion would overflow target type in $convert with no onError value: ",
                longValue >= std::numeric_limits<int>::min() &&
                    longValue <= std::numeric_limits<int>::max());

        return Value(static_cast<int>(longValue));
    }

    static Value performCastNumberToDate(ExpressionContext* const expCtx, Value inputValue) {
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

    static Value performFormatDouble(ExpressionContext* const expCtx, Value inputValue) {
        double doubleValue = inputValue.getDouble();

        if (std::isinf(doubleValue)) {
            return Value(std::signbit(doubleValue) ? "-Infinity"_sd : "Infinity"_sd);
        } else if (std::isnan(doubleValue)) {
            return Value("NaN"_sd);
        } else if (doubleValue == 0.0 && std::signbit(doubleValue)) {
            return Value("-0"_sd);
        } else {
            return Value(static_cast<std::string>(str::stream() << fmt::format("{}", doubleValue)));
        }
    }

    template <class targetType, int base>
    static Value parseStringToNumber(ExpressionContext* const expCtx, Value inputValue) {
        auto stringValue = inputValue.getStringData();
        targetType result;

        // Reject any strings in hex format. This check is needed because the
        // NumberParser::parse call below allows an input hex string prefixed by '0x' when
        // parsing to a double.
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Illegal hexadecimal input in $convert with no onError value: "
                              << stringValue,
                !stringValue.startsWith("0x"));

        Status parseStatus = NumberParser().base(base)(stringValue, &result);
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Failed to parse number '" << stringValue
                              << "' in $convert with no onError value: " << parseStatus.reason(),
                parseStatus.isOK());

        return Value(result);
    }

    static Value parseStringToOID(ExpressionContext* const expCtx, Value inputValue) {
        try {
            return Value(OID::createFromString(inputValue.getStringData()));
        } catch (const DBException& ex) {
            // Rethrow any caught exception as a conversion failure such that 'onError' is evaluated
            // and returned.
            uasserted(ErrorCodes::ConversionFailure,
                      str::stream() << "Failed to parse objectId '" << inputValue.getString()
                                    << "' in $convert with no onError value: " << ex.reason());
        }
    }

    template <typename Func, typename... ExtraArgs>
    static ConversionFunc makeConvertWithExtraArgs(Func&& func, ExtraArgs&&... extraArgs) {
        tassert(4341110, "Conversion function can't be null", func);

        return [=](ExpressionContext* const expCtx, Value inputValue) {
            return func(expCtx, inputValue, std::move(extraArgs)...);
        };
    }

    static Value parseStringToBinData(ExpressionContext* const expCtx,
                                      Value inputValue,
                                      FormatArg format,
                                      SubtypeArg subtypeValue) {
        auto input = inputValue.getStringData();
        auto binDataType = computeBinDataType(subtypeValue);

        try {
            uassert(4341116,
                    "Only the 'uuid' format is allowed with the UUID subtype",
                    (format == BinDataFormat::kUuid) == (binDataType == BinDataType::newUUID));

            switch (format) {
                case BinDataFormat::kBase64: {
                    auto decoded = base64::decode(input);
                    return Value(BSONBinData(decoded.data(), decoded.size(), binDataType));
                }
                case BinDataFormat::kBase64Url: {
                    auto decoded = base64url::decode(input);
                    return Value(BSONBinData(decoded.data(), decoded.size(), binDataType));
                }
                case BinDataFormat::kHex: {
                    auto decoded = hexblob::decode(input);
                    return Value(BSONBinData(decoded.data(), decoded.size(), binDataType));
                }
                case BinDataFormat::kUtf8: {
                    uassert(
                        4341119, str::stream() << "Invalid UTF-8: " << input, isValidUTF8(input));

                    auto decoded = input.toString();
                    return Value(BSONBinData(decoded.data(), decoded.size(), binDataType));
                }
                case BinDataFormat::kUuid: {
                    auto uuid = uassertStatusOK(UUID::parse(input));
                    return Value(uuid);
                }
                default:
                    uasserted(4341117,
                              str::stream() << "Invalid format '" << toStringData(format) << "'");
            }
        } catch (const DBException& ex) {
            uasserted(ErrorCodes::ConversionFailure,
                      str::stream() << "Failed to parse BinData '" << inputValue.getString()
                                    << "' in $convert with no onError value: " << ex.reason());
        }
    }

    static Value performConvertToTrue(ExpressionContext* const expCtx, Value inputValue) {
        return Value(true);
    }

    static Value performIdentityConversion(ExpressionContext* const expCtx, Value inputValue) {
        return inputValue;
    }

    static Value performConvertBinDataToString(ExpressionContext* const expCtx,
                                               Value inputValue,
                                               FormatArg format) {
        try {
            auto binData = inputValue.getBinData();
            bool isValidUuid =
                binData.type == BinDataType::newUUID && binData.length == UUID::kNumBytes;

            switch (format) {
                case BinDataFormat::kAuto: {
                    if (isValidUuid) {
                        // If the BinData represents a valid UUID, return the UUID string.
                        return Value(inputValue.getUuid().toString());
                    }
                    // Otherwise, default to base64.
                    [[fallthrough]];
                }
                case BinDataFormat::kBase64: {
                    auto encoded =
                        base64::encode(binData.data, static_cast<size_t>(binData.length));
                    return Value(encoded);
                }
                case BinDataFormat::kBase64Url: {
                    auto encoded =
                        base64url::encode(binData.data, static_cast<size_t>(binData.length));
                    return Value(encoded);
                }
                case BinDataFormat::kHex: {
                    auto encoded =
                        hexblob::encode(binData.data, static_cast<size_t>(binData.length));
                    return Value(encoded);
                }
                case BinDataFormat::kUtf8: {
                    auto encoded = StringData{static_cast<const char*>(binData.data),
                                              static_cast<size_t>(binData.length)};
                    uassert(4341122,
                            "BinData does not represent a valid UTF-8 string",
                            isValidUTF8(encoded));
                    return Value(encoded);
                }
                case BinDataFormat::kUuid: {
                    uassert(4341121, "BinData does not represent a valid UUID", isValidUuid);
                    return Value(inputValue.getUuid().toString());
                }
                default:
                    uasserted(4341120,
                              str::stream() << "Invalid format '" << toStringData(format) << "'");
            }
        } catch (const DBException& ex) {
            uasserted(ErrorCodes::ConversionFailure,
                      str::stream()
                          << "Failed to convert '" << inputValue.toString()
                          << "' to string in $convert with no onError value: " << ex.reason());
        }
    }

    static Value performConvertBinDataToBinData(ExpressionContext* const expCtx,
                                                Value inputValue,
                                                SubtypeArg subtypeValue) {
        auto binData = inputValue.getBinData();
        uassert(ErrorCodes::ConversionFailure,
                "Conversions between different BinData subtypes are not supported",
                binData.type == computeBinDataType(subtypeValue));

        return Value(BSONBinData{binData.data, binData.length, binData.type});
    }

    template <class ReturnType, class SizeClass>
    static ReturnType readNumberAccordingToEndianness(const ConstDataView& dataView,
                                                      ConvertByteOrderType byteOrder) {
        switch (byteOrder) {
            case ConvertByteOrderType::little:
                return dataView.read<LittleEndian<SizeClass>>();
            case ConvertByteOrderType::big:
                return dataView.read<BigEndian<SizeClass>>();
            default:
                MONGO_UNREACHABLE_TASSERT(9130003);
        }
    }

    template <class ReturnType, class... SizeClass>
    static ReturnType readSizedNumberFromBinData(const BSONBinData& binData,
                                                 ConvertByteOrderType byteOrder) {
        ConstDataView dataView(static_cast<const char*>(binData.data));
        boost::optional<ReturnType> result;
        ((result = sizeof(SizeClass) == binData.length
              ? readNumberAccordingToEndianness<ReturnType, SizeClass>(dataView, byteOrder)
              : result),
         ...);
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Failed to convert '" << Value(binData).toString()
                              << "' to number in $convert because of invalid length: "
                              << binData.length,
                result.has_value());
        return *result;
    }

    static Value performConvertBinDataToInt(ExpressionContext* const expCtx,
                                            Value inputValue,
                                            ByteOrderArg byteOrder) {
        BSONBinData binData = inputValue.getBinData();
        int result = readSizedNumberFromBinData<int /* Return type */, int8_t, int16_t, int32_t>(
            binData, byteOrder);
        return Value(result);
    }

    static Value performConvertBinDataToLong(ExpressionContext* const expCtx,
                                             Value inputValue,
                                             ByteOrderArg byteOrder) {
        BSONBinData binData = inputValue.getBinData();
        long long result = readSizedNumberFromBinData<long long /* Return type */,
                                                      int8_t,
                                                      int16_t,
                                                      int32_t,
                                                      int64_t>(binData, byteOrder);
        return Value(result);
    }

    static Value performConvertBinDataToDouble(ExpressionContext* const expCtx,
                                               Value inputValue,
                                               ByteOrderArg byteOrder) {
        static_assert(sizeof(double) == 8);
        static_assert(sizeof(float) == 4);
        BSONBinData binData = inputValue.getBinData();
        double result =
            readSizedNumberFromBinData<double /* Return type */, float, double>(binData, byteOrder);
        return Value(result);
    }

    template <class ValueType>
    static Value writeNumberAccordingToEndianness(ValueType inputValue,
                                                  ConvertByteOrderType byteOrder,
                                                  SubtypeArg subtypeValue) {
        auto binDataType = computeBinDataType(subtypeValue);
        std::array<char, sizeof(ValueType)> valBytes;
        DataView dataView(valBytes.data());
        switch (byteOrder) {
            case ByteOrderArg::big:
                dataView.write<BigEndian<ValueType>>(inputValue);
                break;
            case ByteOrderArg::little:
                dataView.write<LittleEndian<ValueType>>(inputValue);
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(9130005);
        };
        return Value(BSONBinData{valBytes.data(), static_cast<int>(valBytes.size()), binDataType});
    }

    static Value performConvertIntToBinData(ExpressionContext* const expCtx,
                                            Value inputValue,
                                            ByteOrderArg byteOrder,
                                            SubtypeArg subtypeValue) {
        return writeNumberAccordingToEndianness<int32_t>(
            inputValue.getInt(), byteOrder, subtypeValue);
    }

    static Value performConvertLongToBinData(ExpressionContext* const expCtx,
                                             Value inputValue,
                                             ByteOrderArg byteOrder,
                                             SubtypeArg subtypeValue) {
        return writeNumberAccordingToEndianness<int64_t>(
            inputValue.getLong(), byteOrder, subtypeValue);
    }

    static Value performConvertDoubleToBinData(ExpressionContext* const expCtx,
                                               Value inputValue,
                                               ByteOrderArg byteOrder,
                                               SubtypeArg subtypeValue) {
        return writeNumberAccordingToEndianness<double>(
            inputValue.getDouble(), byteOrder, subtypeValue);
    }

    static bool isValidUserDefinedBinDataType(int typeCode) {
        static const auto smallestUserDefinedType = BinDataType::bdtCustom;
        static const auto largestUserDefinedType = static_cast<BinDataType>(255);
        return (smallestUserDefinedType <= typeCode) && (typeCode <= largestUserDefinedType);
    }

    static BinDataType computeBinDataType(Value subtypeValue) {
        if (subtypeValue.numeric()) {
            uassert(4341106,
                    "In $convert, numeric 'subtype' argument is not an integer",
                    subtypeValue.integral());

            int typeCode = subtypeValue.coerceToInt();
            uassert(4341107,
                    str::stream() << "In $convert, numeric value for 'subtype' does not correspond "
                                     "to a BinData type: "
                                  << typeCode,
                    // Allowed ranges are 0-8 (pre-defined types) and 128-255 (user-defined types).
                    isValidBinDataType(typeCode) || isValidUserDefinedBinDataType(typeCode));

            return static_cast<BinDataType>(typeCode);
        }

        uasserted(
            4341108,
            str::stream() << "For BinData, $convert's 'subtype' argument must be a number, but is "
                          << typeName(subtypeValue.getType()));
    }
};

Expression::Parser makeConversionAlias(
    const StringData shortcutName,
    BSONType toType,
    boost::optional<BinDataFormat> format = boost::none,
    boost::optional<BinDataType> toSubtype = boost::none,
    boost::optional<ConvertByteOrderType> byteOrder = boost::none) {
    return [=](ExpressionContext* const expCtx,
               BSONElement elem,
               const VariablesParseState& vps) -> intrusive_ptr<Expression> {
        // Use parseArguments to allow for a singleton array, or the unwrapped version.
        auto operands = ExpressionNary::parseArguments(expCtx, elem, vps);

        uassert(50723,
                str::stream() << shortcutName << " requires a single argument, got "
                              << operands.size(),
                operands.size() == 1);


        return ExpressionConvert::create(
            expCtx,
            std::move(operands[0]),
            toType,
            // The 'format' argument to $convert is not allowed in FCVs below 8.0. On a newer
            // binary, $toString will still specify it.
            ExpressionConvert::checkBinDataConvertAllowed() ? format : boost::none,
            toSubtype);
    };
}

boost::optional<BinDataFormat> parseBinDataFormat(Value formatValue) {
    if (formatValue.nullish()) {
        return {};
    }

    uassert(4341114,
            str::stream() << "$convert requires that 'format' be a string, found: "
                          << typeName(formatValue.getType()) << " with value "
                          << formatValue.toString(),
            formatValue.getType() == BSONType::String);

    static const StringDataMap<BinDataFormat> stringToBinDataFormat{
        {toStringData(BinDataFormat::kAuto), BinDataFormat::kAuto},
        {toStringData(BinDataFormat::kBase64), BinDataFormat::kBase64},
        {toStringData(BinDataFormat::kBase64Url), BinDataFormat::kBase64Url},
        {toStringData(BinDataFormat::kHex), BinDataFormat::kHex},
        {toStringData(BinDataFormat::kUtf8), BinDataFormat::kUtf8},
        {toStringData(BinDataFormat::kUuid), BinDataFormat::kUuid},
    };

    auto formatString = formatValue.getStringData();
    auto formatPair = stringToBinDataFormat.find(formatString);

    uassert(4341125,
            str::stream() << "Invalid 'format' argument for $convert: " << formatString,
            formatPair != stringToBinDataFormat.end());

    return formatPair->second;
}

boost::optional<ConvertByteOrderType> parseByteOrder(Value byteOrderValue) {
    if (byteOrderValue.nullish()) {
        return {};
    }

    uassert(9130001,
            str::stream() << "$convert requires that 'byteOrder' be a string, found: "
                          << typeName(byteOrderValue.getType()) << " with value "
                          << byteOrderValue.toString(),
            byteOrderValue.getType() == BSONType::String);

    static const StringDataMap<ConvertByteOrderType> stringToByteOrder{
        {toStringData(ConvertByteOrderType::big), ConvertByteOrderType::big},
        {toStringData(ConvertByteOrderType::little), ConvertByteOrderType::little},
    };

    auto byteOrderString = byteOrderValue.getStringData();
    auto byteOrderPair = stringToByteOrder.find(byteOrderString);

    uassert(9130002,
            str::stream() << "Invalid 'byteOrder' argument for $convert: " << byteOrderString,
            byteOrderPair != stringToByteOrder.end());

    return byteOrderPair->second;
}

}  // namespace

REGISTER_STABLE_EXPRESSION(convert, ExpressionConvert::parse);

// Also register shortcut expressions like $toInt, $toString, etc. which can be used as a shortcut
// for $convert without an 'onNull' or 'onError'.
REGISTER_STABLE_EXPRESSION(
    toString, makeConversionAlias("$toString"_sd, BSONType::String, BinDataFormat::kAuto));
REGISTER_STABLE_EXPRESSION(toObjectId, makeConversionAlias("$toObjectId"_sd, BSONType::jstOID));
REGISTER_STABLE_EXPRESSION(toDate, makeConversionAlias("$toDate"_sd, BSONType::Date));
REGISTER_STABLE_EXPRESSION(toDouble, makeConversionAlias("$toDouble"_sd, BSONType::NumberDouble));
REGISTER_STABLE_EXPRESSION(toInt, makeConversionAlias("$toInt"_sd, BSONType::NumberInt));
REGISTER_STABLE_EXPRESSION(toLong, makeConversionAlias("$toLong"_sd, BSONType::NumberLong));
REGISTER_STABLE_EXPRESSION(toDecimal,
                           makeConversionAlias("$toDecimal"_sd, BSONType::NumberDecimal));
REGISTER_STABLE_EXPRESSION(toBool, makeConversionAlias("$toBool"_sd, BSONType::Bool));
REGISTER_EXPRESSION_WITH_FEATURE_FLAG(toUUID,
                                      makeConversionAlias("$toUUID"_sd,
                                                          BSONType::BinData,
                                                          BinDataFormat::kUuid,
                                                          BinDataType::newUUID),
                                      AllowedWithApiStrict::kAlways,
                                      AllowedWithClientType::kAny,
                                      feature_flags::gFeatureFlagBinDataConvert);

boost::intrusive_ptr<Expression> ExpressionConvert::create(
    ExpressionContext* const expCtx,
    boost::intrusive_ptr<Expression> input,
    BSONType toType,
    boost::optional<BinDataFormat> format,
    boost::optional<BinDataType> toSubtype,
    boost::optional<ConvertByteOrderType> byteOrder) {
    auto targetType = StringData(typeName(toType));
    auto toValue = toSubtype
        ? Value(BSON("type" << targetType << "subtype" << static_cast<int>(*toSubtype)))
        : Value(targetType);

    return new ExpressionConvert(
        expCtx,
        std::move(input),
        ExpressionConstant::create(expCtx, std::move(toValue)),
        format ? ExpressionConstant::create(expCtx, Value(toStringData(*format))) : nullptr,
        nullptr,
        nullptr,
        byteOrder ? ExpressionConstant::create(expCtx, Value(toStringData(*byteOrder))) : nullptr,
        checkBinDataConvertAllowed(),
        checkBinDataConvertNumericAllowed());
}

ExpressionConvert::ExpressionConvert(ExpressionContext* const expCtx,
                                     boost::intrusive_ptr<Expression> input,
                                     boost::intrusive_ptr<Expression> to,
                                     boost::intrusive_ptr<Expression> format,
                                     boost::intrusive_ptr<Expression> onError,
                                     boost::intrusive_ptr<Expression> onNull,
                                     boost::intrusive_ptr<Expression> byteOrder,
                                     const bool allowBinDataConvert,
                                     const bool allowBinDataConvertNumeric)
    : Expression(expCtx,
                 {std::move(input),
                  std::move(to),
                  std::move(format),
                  std::move(onError),
                  std::move(onNull),
                  std::move(byteOrder)}),
      _allowBinDataConvert{allowBinDataConvert},
      _allowBinDataConvertNumeric{allowBinDataConvertNumeric} {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

intrusive_ptr<Expression> ExpressionConvert::parse(ExpressionContext* const expCtx,
                                                   BSONElement expr,
                                                   const VariablesParseState& vps) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$convert expects an object of named arguments but found: "
                          << typeName(expr.type()),
            expr.type() == BSONType::Object);

    const bool allowBinDataConvert = checkBinDataConvertAllowed();
    const bool allowBinDataConvertNumeric = checkBinDataConvertNumericAllowed();

    boost::intrusive_ptr<Expression> input;
    boost::intrusive_ptr<Expression> to;
    boost::intrusive_ptr<Expression> format;
    boost::intrusive_ptr<Expression> onError;
    boost::intrusive_ptr<Expression> onNull;
    boost::intrusive_ptr<Expression> byteOrder;
    for (auto&& elem : expr.embeddedObject()) {
        const auto field = elem.fieldNameStringData();
        if (field == "input"_sd) {
            input = parseOperand(expCtx, elem, vps);
        } else if (field == "to"_sd) {
            to = parseOperand(expCtx, elem, vps);
        } else if (field == "format"_sd) {
            uassert(
                ErrorCodes::FailedToParse,
                str::stream() << "The 'format' argument to $convert is not allowed in the "
                                 "current feature compatibility version. See "
                              << feature_compatibility_version_documentation::compatibilityLink()
                              << ".",
                // If the command came from router, it means router must be on an FCV that
                // supports the 'format' field.
                expCtx->getFromRouter() || allowBinDataConvert);
            format = parseOperand(expCtx, elem, vps);
        } else if (field == "onError"_sd) {
            onError = parseOperand(expCtx, elem, vps);
        } else if (field == "onNull"_sd) {
            onNull = parseOperand(expCtx, elem, vps);
        } else if (field == "byteOrder"_sd) {
            uassert(
                ErrorCodes::FailedToParse,
                str::stream() << "The 'byteOrder' argument to $convert is not allowed in the "
                                 "current feature compatibility version. See "
                              << feature_compatibility_version_documentation::compatibilityLink()
                              << ".",
                allowBinDataConvertNumeric);
            byteOrder = parseOperand(expCtx, elem, vps);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "$convert found an unknown argument: " << elem.fieldNameStringData());
        }
    }

    uassert(ErrorCodes::FailedToParse, "Missing 'input' parameter to $convert", input);
    uassert(ErrorCodes::FailedToParse, "Missing 'to' parameter to $convert", to);

    return new ExpressionConvert(expCtx,
                                 std::move(input),
                                 std::move(to),
                                 std::move(format),
                                 std::move(onError),
                                 std::move(onNull),
                                 std::move(byteOrder),
                                 allowBinDataConvert,
                                 allowBinDataConvertNumeric);
}

boost::optional<ExpressionConvert::ConvertTargetTypeInfo>
ExpressionConvert::ConvertTargetTypeInfo::parse(Value value) {
    if (value.nullish()) {
        return {};
    }

    // We expect 'to' to be either:
    // - A document describing the target type and subtype.
    // - A string or a numeric value representing a valid BSON type.
    Value typeValue;
    Value subtypeValue;
    if (value.isObject()) {
        typeValue = value["type"_sd];
        subtypeValue = value["subtype"_sd];
    } else {
        typeValue = value;
    }

    if (subtypeValue.missing()) {
        subtypeValue = Value(static_cast<int>(BinDataType::BinDataGeneral));
    }

    auto targetType = computeTargetType(typeValue);
    return ConvertTargetTypeInfo{targetType, subtypeValue};
}

Value ExpressionConvert::evaluate(const Document& root, Variables* variables) const {
    auto toValue = _children[_kTo]->evaluate(root, variables);
    auto inputValue = _children[_kInput]->evaluate(root, variables);
    auto formatValue =
        _children[_kFormat] ? _children[_kFormat]->evaluate(root, variables) : Value();
    auto byteOrderValue =
        _children[_kByteOrder] ? _children[_kByteOrder]->evaluate(root, variables) : Value();

    auto targetTypeInfo = ConvertTargetTypeInfo::parse(toValue);

    if (inputValue.nullish()) {
        return _children[_kOnNull] ? _children[_kOnNull]->evaluate(root, variables)
                                   : Value(BSONNULL);
    }

    if (!targetTypeInfo) {
        // "to" evaluated to a nullish value.
        return Value(BSONNULL);
    }

    auto format = parseBinDataFormat(formatValue);
    auto byteOrder = parseByteOrder(byteOrderValue);

    try {
        return performConversion(*targetTypeInfo, inputValue, format, byteOrder);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (_children[_kOnError]) {
            return _children[_kOnError]->evaluate(root, variables);
        } else {
            throw;
        }
    }
}

boost::intrusive_ptr<Expression> ExpressionConvert::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kTo] = _children[_kTo]->optimize();
    if (_children[_kFormat]) {
        _children[_kFormat] = _children[_kFormat]->optimize();
    }
    if (_children[_kOnError]) {
        _children[_kOnError] = _children[_kOnError]->optimize();
    }
    if (_children[_kOnNull]) {
        _children[_kOnNull] = _children[_kOnNull]->optimize();
    }
    if (_children[_kByteOrder]) {
        _children[_kByteOrder] = _children[_kByteOrder]->optimize();
    }

    // Perform constant folding if possible. This does not support folding for $convert operations
    // that have constant _children[_kTo] and _children[_kInput] values but non-constant
    // _children[_kOnError] and _children[_kOnNull] values. Because _children[_kOnError] and
    // _children[_kOnNull] are evaluated lazily, conversions that do not used the
    // _children[_kOnError] and _children[_kOnNull] values could still be legally folded if those
    // values are not needed. Support for that case would add more complexity than it's worth,
    // though.
    if (ExpressionConstant::allNullOrConstant({_children[_kInput],
                                               _children[_kTo],
                                               _children[_kFormat],
                                               _children[_kOnError],
                                               _children[_kOnNull],
                                               _children[_kByteOrder]})) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }

    return this;
}

Value ExpressionConvert::serialize(const SerializationOptions& options) const {
    // Since the 'to' field is a parameter from a set of valid values and not free user input,
    // we want to avoid boiling it down to the representative value in the query shape. The first
    // condition is so that we can keep serializing correctly whenever the 'to' field is an
    // expression that gets resolved down to a string of a valid type, or its corresponding
    // numerical value. If it's just the constant, we want to wrap it in a $const except when the
    // serialization policy is debug.
    auto constExpr = dynamic_cast<ExpressionConstant*>(_children[_kTo].get());
    Value toField = Value();
    if (!constExpr) {
        toField = _children[_kTo]->serialize(options);
    } else if (options.literalPolicy == LiteralSerializationPolicy::kToDebugTypeString) {
        toField = constExpr->getValue();
    } else {
        toField = Value(DOC("$const" << constExpr->getValue()));
    }
    return Value(Document{
        {"$convert",
         Document{
             {"input", _children[_kInput]->serialize(options)},
             {"to", toField},
             {"format", _children[_kFormat] ? _children[_kFormat]->serialize(options) : Value()},
             {"onError", _children[_kOnError] ? _children[_kOnError]->serialize(options) : Value()},
             {"onNull", _children[_kOnNull] ? _children[_kOnNull]->serialize(options) : Value()},
             {"byteOrder",
              _children[_kByteOrder] ? _children[_kByteOrder]->serialize(options) : Value()}}}});
}

BSONType ExpressionConvert::computeTargetType(Value targetTypeName) {
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

bool ExpressionConvert::requestingConvertBinDataNumeric(ConvertTargetTypeInfo targetTypeInfo,
                                                        BSONType inputType) const {
    return (inputType == BSONType::BinData &&
            (targetTypeInfo.type == BSONType::NumberInt ||
             targetTypeInfo.type == BSONType::NumberLong ||
             targetTypeInfo.type == BSONType::NumberDouble)) ||
        ((inputType == BSONType::NumberInt || inputType == BSONType::NumberLong ||
          inputType == BSONType::NumberDouble) &&
         targetTypeInfo.type == BSONType::BinData);
}

Value ExpressionConvert::performConversion(ConvertTargetTypeInfo targetTypeInfo,
                                           Value inputValue,
                                           boost::optional<BinDataFormat> format,
                                           boost::optional<ConvertByteOrderType> byteOrder) const {
    invariant(!inputValue.nullish());

    static const ConversionTable table;
    BSONType inputType = inputValue.getType();

    uassert(ErrorCodes::ConversionFailure,
            str::stream() << "BinData $convert is not allowed in the current feature "
                             "compatibility version. See "
                          << feature_compatibility_version_documentation::compatibilityLink()
                          << ".",
            _allowBinDataConvert || targetTypeInfo.type == BSONType::Bool ||
                (inputType != BSONType::BinData && targetTypeInfo.type != BSONType::BinData));

    uassert(ErrorCodes::ConversionFailure,
            str::stream()
                << "BinData $convert with numeric values is not allowed in the current feature "
                   "compatibility version. See "
                << feature_compatibility_version_documentation::compatibilityLink() << ".",
            _allowBinDataConvertNumeric ||
                !requestingConvertBinDataNumeric(targetTypeInfo, inputType));

    return table.findConversionFunc(
        inputType, targetTypeInfo.type, format, targetTypeInfo.subtype, byteOrder)(
        getExpressionContext(), inputValue);
}

bool ExpressionConvert::checkBinDataConvertAllowed() {
    return feature_flags::gFeatureFlagBinDataConvert.isEnabledUseLatestFCVWhenUninitialized(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

bool ExpressionConvert::checkBinDataConvertNumericAllowed() {
    return feature_flags::gFeatureFlagBinDataConvertNumeric.isEnabledUseLatestFCVWhenUninitialized(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

namespace {

auto CommonRegexParse(ExpressionContext* const expCtx,
                      BSONElement expr,
                      const VariablesParseState& vpsIn,
                      StringData opName) {
    uassert(51103,
            str::stream() << opName
                          << " expects an object of named arguments but found: " << expr.type(),
            expr.type() == BSONType::Object);

    struct {
        boost::intrusive_ptr<Expression> input;
        boost::intrusive_ptr<Expression> regex;
        boost::intrusive_ptr<Expression> options;
    } parsed;
    for (auto&& elem : expr.embeddedObject()) {
        const auto field = elem.fieldNameStringData();
        if (field == "input"_sd) {
            parsed.input = Expression::parseOperand(expCtx, elem, vpsIn);
        } else if (field == "regex"_sd) {
            parsed.regex = Expression::parseOperand(expCtx, elem, vpsIn);
        } else if (field == "options"_sd) {
            parsed.options = Expression::parseOperand(expCtx, elem, vpsIn);
        } else {
            uasserted(31024,
                      str::stream() << opName << " found an unknown argument: "
                                    << elem.fieldNameStringData());
        }
    }
    uassert(31022, str::stream() << opName << " requires 'input' parameter", parsed.input);
    uassert(31023, str::stream() << opName << " requires 'regex' parameter", parsed.regex);

    return parsed;
}

}  // namespace

/* -------------------------- ExpressionRegex ------------------------------ */

ExpressionRegex::RegexExecutionState ExpressionRegex::buildInitialState(
    const Document& root, Variables* variables) const {
    Value textInput = _children[_kInput]->evaluate(root, variables);
    Value regexPattern = _children[_kRegex]->evaluate(root, variables);
    Value regexOptions =
        _children[_kOptions] ? _children[_kOptions]->evaluate(root, variables) : Value(BSONNULL);

    auto executionState = _initialExecStateForConstantRegex.value_or(RegexExecutionState());

    // The 'input' parameter can be a variable and needs to be extracted from the expression
    // document even when '_preExecutionState' is present.
    _extractInputField(&executionState, textInput);

    // If we have a prebuilt execution state, then the 'regex' and 'options' fields are constant
    // values, and we do not need to re-compile them.
    if (!hasConstantRegex()) {
        _extractRegexAndOptions(&executionState, regexPattern, regexOptions);
        _compile(&executionState);
    }

    return executionState;
}

pcre::MatchData ExpressionRegex::execute(RegexExecutionState* regexState) const {
    invariant(regexState);
    invariant(!regexState->nullish());
    invariant(regexState->pcrePtr);

    StringData in = *regexState->input;
    auto m = regexState->pcrePtr->matchView(in, {}, regexState->startBytePos);
    uassert(51156,
            str::stream() << "Error occurred while executing the regular expression in " << _opName
                          << ". Result code: " << errorMessage(m.error()),
            m || m.error() == pcre::Errc::ERROR_NOMATCH);
    return m;
}

Value ExpressionRegex::nextMatch(RegexExecutionState* regexState) const {
    auto m = execute(regexState);
    if (!m)
        // No match.
        return Value(BSONNULL);

    auto afterStart = m.input().substr(m.startPos());
    auto beforeMatch = afterStart.substr(0, m[0].data() - afterStart.data());
    regexState->startCodePointPos += str::lengthInUTF8CodePoints(beforeMatch);

    // Set the start index for match to the new one.
    regexState->startBytePos = m[0].data() - m.input().data();

    std::vector<Value> captures;
    captures.reserve(m.captureCount());

    for (size_t i = 1; i < m.captureCount() + 1; ++i) {
        if (StringData cap = m[i]; !cap.rawData()) {
            // Use BSONNULL placeholder for unmatched capture groups.
            captures.push_back(Value(BSONNULL));
        } else {
            captures.push_back(Value(cap));
        }
    }

    MutableDocument match;
    match.addField("match", Value(m[0]));
    match.addField("idx", Value(regexState->startCodePointPos));
    match.addField("captures", Value(std::move(captures)));
    return match.freezeToValue();
}

boost::intrusive_ptr<Expression> ExpressionRegex::optimize() {
    _children[_kInput] = _children[_kInput]->optimize();
    _children[_kRegex] = _children[_kRegex]->optimize();
    if (_children[_kOptions]) {
        _children[_kOptions] = _children[_kOptions]->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_children[_kRegex], _children[_kOptions]})) {
        _initialExecStateForConstantRegex.emplace();
        _extractRegexAndOptions(
            _initialExecStateForConstantRegex.get_ptr(),
            static_cast<ExpressionConstant*>(_children[_kRegex].get())->getValue(),
            _children[_kOptions]
                ? static_cast<ExpressionConstant*>(_children[_kOptions].get())->getValue()
                : Value());
        _compile(_initialExecStateForConstantRegex.get_ptr());
    }
    return this;
}

void ExpressionRegex::_compile(RegexExecutionState* executionState) const {
    if (!executionState->pattern) {
        return;
    }

    auto re = std::make_shared<pcre::Regex>(
        *executionState->pattern,
        pcre_util::flagsToOptions(executionState->options.value_or(""), _opName));
    uassert(51111,
            str::stream() << "Invalid Regex in " << _opName << ": " << errorMessage(re->error()),
            *re);
    executionState->pcrePtr = std::move(re);

    // Calculate the number of capture groups present in 'pattern' and store in 'numCaptures'.
    executionState->numCaptures = executionState->pcrePtr->captureCount();
}

Value ExpressionRegex::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {_opName,
         Document{{"input", _children[_kInput]->serialize(options)},
                  {"regex", _children[_kRegex]->serialize(options)},
                  {"options",
                   _children[_kOptions] ? _children[_kOptions]->serialize(options) : Value()}}}});
}

void ExpressionRegex::_extractInputField(RegexExecutionState* executionState,
                                         const Value& textInput) const {
    uassert(51104,
            str::stream() << _opName << " needs 'input' to be of type string",
            textInput.nullish() || textInput.getType() == BSONType::String);
    if (textInput.getType() == BSONType::String) {
        executionState->input = textInput.getString();
    }
}

void ExpressionRegex::_extractRegexAndOptions(RegexExecutionState* executionState,
                                              const Value& regexPattern,
                                              const Value& regexOptions) const {
    uassert(51105,
            str::stream() << _opName << " needs 'regex' to be of type string or regex",
            regexPattern.nullish() || regexPattern.getType() == BSONType::String ||
                regexPattern.getType() == BSONType::RegEx);
    uassert(51106,
            str::stream() << _opName << " needs 'options' to be of type string",
            regexOptions.nullish() || regexOptions.getType() == BSONType::String);

    // The 'regex' field can be a RegEx object and may have its own options...
    if (regexPattern.getType() == BSONType::RegEx) {
        StringData regexFlags = regexPattern.getRegexFlags();
        executionState->pattern = regexPattern.getRegex();
        uassert(51107,
                str::stream()
                    << _opName
                    << ": found regex option(s) specified in both 'regex' and 'option' fields",
                regexOptions.nullish() || regexFlags.empty());
        if (!regexFlags.empty()) {
            executionState->options = regexFlags.toString();
        }
    } else if (regexPattern.getType() == BSONType::String) {
        // ...or it can be a string field with options specified separately.
        executionState->pattern = regexPattern.getString();
    }

    // If 'options' is non-null, we must validate its contents even if 'regexPattern' is nullish.
    if (!regexOptions.nullish()) {
        executionState->options = regexOptions.getString();
    }
    uassert(51109,
            str::stream() << _opName << ": regular expression cannot contain an embedded null byte",
            !executionState->pattern ||
                executionState->pattern->find('\0', 0) == std::string::npos);

    uassert(51110,
            str::stream() << _opName
                          << ": regular expression options cannot contain an embedded null byte",
            !executionState->options ||
                executionState->options->find('\0', 0) == std::string::npos);
}

boost::optional<std::pair<boost::optional<std::string>, std::string>>
ExpressionRegex::getConstantPatternAndOptions() const {
    if (!ExpressionConstant::isNullOrConstant(_children[_kRegex]) ||
        !ExpressionConstant::isNullOrConstant(_children[_kOptions])) {
        return {};
    }
    auto patternValue = static_cast<ExpressionConstant*>(_children[_kRegex].get())->getValue();
    uassert(5073405,
            str::stream() << _opName << " needs 'regex' to be of type string or regex",
            patternValue.nullish() || patternValue.getType() == BSONType::RegEx ||
                patternValue.getType() == BSONType::String);
    auto patternStr = [&]() -> boost::optional<std::string> {
        if (patternValue.getType() == BSONType::RegEx) {
            StringData flags = patternValue.getRegexFlags();
            uassert(5073406,
                    str::stream()
                        << _opName
                        << ": found regex options specified in both 'regex' and 'options' fields",
                    _children[_kOptions].get() == nullptr || flags.empty());
            return std::string(patternValue.getRegex());
        } else if (patternValue.getType() == BSONType::String) {
            return patternValue.getString();
        } else {
            return boost::none;
        }
    }();

    auto optionsStr = [&]() -> std::string {
        if (_children[_kOptions].get() != nullptr) {
            auto optValue =
                static_cast<ExpressionConstant*>(_children[_kOptions].get())->getValue();
            uassert(5126607,
                    str::stream() << _opName << " needs 'options' to be of type string",
                    optValue.nullish() || optValue.getType() == BSONType::String);
            if (optValue.getType() == BSONType::String) {
                return optValue.getString();
            }
        }
        if (patternValue.getType() == BSONType::RegEx) {
            StringData flags = patternValue.getRegexFlags();
            if (!flags.empty()) {
                return flags.toString();
            }
        }
        return {};
    }();

    uassert(5073407,
            str::stream() << _opName << ": regular expression cannot contain an embedded null byte",
            !patternStr || patternStr->find('\0') == std::string::npos);

    uassert(5073408,
            str::stream() << _opName
                          << ": regular expression options cannot contain an embedded null byte",
            optionsStr.find('\0') == std::string::npos);

    return std::make_pair(patternStr, optionsStr);
}

/* -------------------------- ExpressionRegexFind ------------------------------ */

REGISTER_STABLE_EXPRESSION(regexFind, ExpressionRegexFind::parse);
boost::intrusive_ptr<Expression> ExpressionRegexFind::parse(ExpressionContext* const expCtx,
                                                            BSONElement expr,
                                                            const VariablesParseState& vpsIn) {
    auto opName = "$regexFind"_sd;
    auto [input, regex, options] = CommonRegexParse(expCtx, expr, vpsIn, opName);
    return new ExpressionRegexFind(
        expCtx, std::move(input), std::move(regex), std::move(options), opName);
}

Value ExpressionRegexFind::evaluate(const Document& root, Variables* variables) const {
    auto executionState = buildInitialState(root, variables);
    if (executionState.nullish()) {
        return Value(BSONNULL);
    }
    return nextMatch(&executionState);
}

/* -------------------------- ExpressionRegexFindAll ------------------------------ */

REGISTER_STABLE_EXPRESSION(regexFindAll, ExpressionRegexFindAll::parse);
boost::intrusive_ptr<Expression> ExpressionRegexFindAll::parse(ExpressionContext* const expCtx,
                                                               BSONElement expr,
                                                               const VariablesParseState& vpsIn) {
    auto opName = "$regexFindAll"_sd;
    auto [input, regex, options] = CommonRegexParse(expCtx, expr, vpsIn, opName);
    return new ExpressionRegexFindAll(
        expCtx, std::move(input), std::move(regex), std::move(options), opName);
}

Value ExpressionRegexFindAll::evaluate(const Document& root, Variables* variables) const {
    std::vector<Value> output;
    auto executionState = buildInitialState(root, variables);
    if (executionState.nullish()) {
        return Value(std::move(output));
    }
    StringData input = *(executionState.input);
    size_t totalDocSize = 0;

    // Using do...while loop because, when input is an empty string, we still want to see if there
    // is a match.
    do {
        auto matchObj = nextMatch(&executionState);
        if (matchObj.getType() == BSONType::jstNULL) {
            break;
        }
        totalDocSize += matchObj.getApproximateSize();
        uassert(51151,
                str::stream() << getOpName()
                              << ": the size of buffer to store output exceeded the 64MB limit",
                totalDocSize <= mongo::BufferMaxSize);

        output.push_back(matchObj);
        std::string matchStr = matchObj.getDocument().getField("match").getString();
        if (matchStr.empty()) {
            // This would only happen if the regex matched an empty string. In this case, even if
            // the character at startByteIndex matches the regex, we cannot return it since we are
            // already returing an empty string starting at this index. So we move on to the next
            // byte index.
            if (static_cast<size_t>(executionState.startBytePos) >= input.size())
                continue;  // input already exhausted
            executionState.startBytePos +=
                str::getCodePointLength(input[executionState.startBytePos]);
            ++executionState.startCodePointPos;
            continue;
        }

        // We don't want any overlapping sub-strings. So we move 'startBytePos' to point to the
        // byte after 'matchStr'. We move the code point index also correspondingly.
        executionState.startBytePos += matchStr.size();
        for (size_t byteIx = 0; byteIx < matchStr.size(); ++executionState.startCodePointPos) {
            byteIx += str::getCodePointLength(matchStr[byteIx]);
        }

        invariant(executionState.startBytePos > 0);
        invariant(executionState.startCodePointPos > 0);
        invariant(executionState.startCodePointPos <= executionState.startBytePos);
    } while (static_cast<size_t>(executionState.startBytePos) < input.size());
    return Value(std::move(output));
}

/* -------------------------- ExpressionRegexMatch ------------------------------ */

REGISTER_STABLE_EXPRESSION(regexMatch, ExpressionRegexMatch::parse);
boost::intrusive_ptr<Expression> ExpressionRegexMatch::parse(ExpressionContext* const expCtx,
                                                             BSONElement expr,
                                                             const VariablesParseState& vpsIn) {
    auto opName = "$regexMatch"_sd;
    auto [input, regex, options] = CommonRegexParse(expCtx, expr, vpsIn, opName);
    return new ExpressionRegexMatch(
        expCtx, std::move(input), std::move(regex), std::move(options), opName);
}

Value ExpressionRegexMatch::evaluate(const Document& root, Variables* variables) const {
    auto state = buildInitialState(root, variables);
    if (state.nullish())
        return Value(false);
    pcre::MatchData m = execute(&state);
    return Value(!!m);
}

/* -------------------------- ExpressionRandom ------------------------------ */
REGISTER_STABLE_EXPRESSION(rand, ExpressionRandom::parse);

ExpressionRandom::ExpressionRandom(ExpressionContext* const expCtx) : Expression(expCtx) {}

intrusive_ptr<Expression> ExpressionRandom::parse(ExpressionContext* const expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps) {
    uassert(3040500,
            "$rand not allowed inside collection validators",
            !expCtx->getIsParsingCollectionValidator());

    uassert(3040501, "$rand does not currently accept arguments", exprElement.Obj().isEmpty());

    return new ExpressionRandom(expCtx);
}

const char* ExpressionRandom::getOpName() const {
    return "$rand";
}

double ExpressionRandom::getRandomValue() const {
    return kMinValue + (kMaxValue - kMinValue) * random_utils::getRNG().nextCanonicalDouble();
}

Value ExpressionRandom::evaluate(const Document& root, Variables* variables) const {
    return Value(getRandomValue());
}

intrusive_ptr<Expression> ExpressionRandom::optimize() {
    return intrusive_ptr<Expression>(this);
}

Value ExpressionRandom::serialize(const SerializationOptions& options) const {
    return Value(DOC(getOpName() << Document()));
}

/* ------------------------- ExpressionToHashedIndexKey -------------------------- */
REGISTER_STABLE_EXPRESSION(toHashedIndexKey, ExpressionToHashedIndexKey::parse);

boost::intrusive_ptr<Expression> ExpressionToHashedIndexKey::parse(ExpressionContext* const expCtx,
                                                                   BSONElement expr,
                                                                   const VariablesParseState& vps) {
    return make_intrusive<ExpressionToHashedIndexKey>(expCtx, parseOperand(expCtx, expr, vps));
}

Value ExpressionToHashedIndexKey::evaluate(const Document& root, Variables* variables) const {
    Value inpVal(_children[0]->evaluate(root, variables));
    if (inpVal.missing()) {
        inpVal = Value(BSONNULL);
    }

    return Value(BSONElementHasher::hash64(BSON("" << inpVal).firstElement(),
                                           BSONElementHasher::DEFAULT_HASH_SEED));
}

Value ExpressionToHashedIndexKey::serialize(const SerializationOptions& options) const {
    return Value(DOC("$toHashedIndexKey" << _children[0]->serialize(options)));
}

/* ------------------------- ExpressionDateArithmetics -------------------------- */
namespace {
auto commonDateArithmeticsParse(ExpressionContext* const expCtx,
                                BSONElement expr,
                                const VariablesParseState& vps,
                                StringData opName) {
    uassert(5166400,
            str::stream() << opName << " expects an object as its argument",
            expr.type() == BSONType::Object);

    struct {
        boost::intrusive_ptr<Expression> startDate;
        boost::intrusive_ptr<Expression> unit;
        boost::intrusive_ptr<Expression> amount;
        boost::intrusive_ptr<Expression> timezone;
    } parsedArgs;

    const BSONObj args = expr.embeddedObject();
    for (auto&& arg : args) {
        auto field = arg.fieldNameStringData();

        if (field == "startDate"_sd) {
            parsedArgs.startDate = Expression::parseOperand(expCtx, arg, vps);
        } else if (field == "unit"_sd) {
            parsedArgs.unit = Expression::parseOperand(expCtx, arg, vps);
        } else if (field == "amount"_sd) {
            parsedArgs.amount = Expression::parseOperand(expCtx, arg, vps);
        } else if (field == "timezone"_sd) {
            parsedArgs.timezone = Expression::parseOperand(expCtx, arg, vps);
        } else {
            uasserted(5166401,
                      str::stream()
                          << "Unrecognized argument to " << opName << ": " << arg.fieldName()
                          << ". Expected arguments are startDate, unit, amount, and optionally "
                             "timezone.");
        }
    }
    uassert(5166402,
            str::stream() << opName << " requires startDate, unit, and amount to be present",
            parsedArgs.startDate && parsedArgs.unit && parsedArgs.amount);

    return parsedArgs;
}
}  // namespace

boost::intrusive_ptr<Expression> ExpressionDateArithmetics::optimize() {
    _children[_kStartDate] = _children[_kStartDate]->optimize();
    _children[_kUnit] = _children[_kUnit]->optimize();
    _children[_kAmount] = _children[_kAmount]->optimize();
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }

    if (ExpressionConstant::allNullOrConstant({_children[_kStartDate],
                                               _children[_kUnit],
                                               _children[_kAmount],
                                               _children[_kTimeZone]})) {
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isConstant(_children[_kUnit])) {
        const Value unitVal =
            _children[_kUnit]->evaluate(Document{}, &(getExpressionContext()->variables));
        if (unitVal.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        _parsedUnit = exec::expression::parseTimeUnit(unitVal, _opName);
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone =
            exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                           Document{},
                                           _children[_kTimeZone].get(),
                                           &(getExpressionContext()->variables));
        if (!_parsedTimeZone) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
    }
    return intrusive_ptr<Expression>(this);
}

Value ExpressionDateArithmetics::serialize(const SerializationOptions& options) const {
    return Value(Document{
        {_opName,
         Document{{"startDate", _children[_kStartDate]->serialize(options)},
                  {"unit", _children[_kUnit]->serialize(options)},
                  {"amount", _children[_kAmount]->serialize(options)},
                  {"timezone",
                   _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value()}}}});
}

monotonic::State ExpressionDateArithmetics::getMonotonicState(
    const FieldPath& sortedFieldPath) const {
    if (!ExpressionConstant::allNullOrConstant({_children[_kUnit], _children[_kTimeZone]})) {
        return monotonic::State::NonMonotonic;
    }
    return combineMonotonicStateOfArguments(
        _children[_kStartDate]->getMonotonicState(sortedFieldPath),
        _children[_kAmount]->getMonotonicState(sortedFieldPath));
}

/* ----------------------- ExpressionDateAdd ---------------------------- */
REGISTER_STABLE_EXPRESSION(dateAdd, ExpressionDateAdd::parse);

boost::intrusive_ptr<Expression> ExpressionDateAdd::parse(ExpressionContext* const expCtx,
                                                          BSONElement expr,
                                                          const VariablesParseState& vps) {
    constexpr auto opName = "$dateAdd"_sd;
    auto [startDate, unit, amount, timezone] =
        commonDateArithmeticsParse(expCtx, expr, vps, opName);
    return make_intrusive<ExpressionDateAdd>(expCtx,
                                             std::move(startDate),
                                             std::move(unit),
                                             std::move(amount),
                                             std::move(timezone),
                                             opName);
}

Value ExpressionDateAdd::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

monotonic::State ExpressionDateAdd::combineMonotonicStateOfArguments(
    monotonic::State startDataMonotonicState, monotonic::State amountMonotonicState) const {
    return monotonic::combine(startDataMonotonicState, amountMonotonicState);
}

/* ----------------------- ExpressionDateSubtract ---------------------------- */

REGISTER_STABLE_EXPRESSION(dateSubtract, ExpressionDateSubtract::parse);

boost::intrusive_ptr<Expression> ExpressionDateSubtract::parse(ExpressionContext* const expCtx,
                                                               BSONElement expr,
                                                               const VariablesParseState& vps) {
    constexpr auto opName = "$dateSubtract"_sd;
    auto [startDate, unit, amount, timezone] =
        commonDateArithmeticsParse(expCtx, expr, vps, opName);
    return make_intrusive<ExpressionDateSubtract>(expCtx,
                                                  std::move(startDate),
                                                  std::move(unit),
                                                  std::move(amount),
                                                  std::move(timezone),
                                                  opName);
}

Value ExpressionDateSubtract::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

monotonic::State ExpressionDateSubtract::combineMonotonicStateOfArguments(
    monotonic::State startDataMonotonicState, monotonic::State amountMonotonicState) const {
    return monotonic::combine(startDataMonotonicState, amountMonotonicState);
}

/* ----------------------- ExpressionDateTrunc ---------------------------- */

REGISTER_STABLE_EXPRESSION(dateTrunc, ExpressionDateTrunc::parse);

ExpressionDateTrunc::ExpressionDateTrunc(ExpressionContext* const expCtx,
                                         boost::intrusive_ptr<Expression> date,
                                         boost::intrusive_ptr<Expression> unit,
                                         boost::intrusive_ptr<Expression> binSize,
                                         boost::intrusive_ptr<Expression> timezone,
                                         boost::intrusive_ptr<Expression> startOfWeek)
    : Expression{expCtx,
                 {std::move(date),
                  std::move(unit),
                  std::move(binSize),
                  std::move(timezone),
                  std::move(startOfWeek)}} {}

boost::intrusive_ptr<Expression> ExpressionDateTrunc::parse(ExpressionContext* const expCtx,
                                                            BSONElement expr,
                                                            const VariablesParseState& vps) {
    tassert(5439011, "Invalid expression passed", expr.fieldNameStringData() == "$dateTrunc");
    uassert(5439007,
            "$dateTrunc only supports an object as its argument",
            expr.type() == BSONType::Object);
    BSONElement dateElement, unitElement, binSizeElement, timezoneElement, startOfWeekElement;
    for (auto&& element : expr.embeddedObject()) {
        auto field = element.fieldNameStringData();
        if ("date"_sd == field) {
            dateElement = element;
        } else if ("binSize"_sd == field) {
            binSizeElement = element;
        } else if ("unit"_sd == field) {
            unitElement = element;
        } else if ("timezone"_sd == field) {
            timezoneElement = element;
        } else if ("startOfWeek"_sd == field) {
            startOfWeekElement = element;
        } else {
            uasserted(5439008,
                      str::stream()
                          << "Unrecognized argument to $dateTrunc: " << element.fieldName()
                          << ". Expected arguments are date, unit, and optionally, binSize, "
                             "timezone, startOfWeek");
        }
    }
    uassert(5439009, "Missing 'date' parameter to $dateTrunc", dateElement);
    uassert(5439010, "Missing 'unit' parameter to $dateTrunc", unitElement);
    return make_intrusive<ExpressionDateTrunc>(
        expCtx,
        parseOperand(expCtx, dateElement, vps),
        parseOperand(expCtx, unitElement, vps),
        binSizeElement ? parseOperand(expCtx, binSizeElement, vps) : nullptr,
        timezoneElement ? parseOperand(expCtx, timezoneElement, vps) : nullptr,
        startOfWeekElement ? parseOperand(expCtx, startOfWeekElement, vps) : nullptr);
}

boost::intrusive_ptr<Expression> ExpressionDateTrunc::optimize() {
    _children[_kDate] = _children[_kDate]->optimize();
    _children[_kUnit] = _children[_kUnit]->optimize();
    if (_children[_kBinSize]) {
        _children[_kBinSize] = _children[_kBinSize]->optimize();
    }
    if (_children[_kTimeZone]) {
        _children[_kTimeZone] = _children[_kTimeZone]->optimize();
    }
    if (_children[_kStartOfWeek]) {
        _children[_kStartOfWeek] = _children[_kStartOfWeek]->optimize();
    }
    if (ExpressionConstant::allNullOrConstant({_children[_kDate],
                                               _children[_kUnit],
                                               _children[_kBinSize],
                                               _children[_kTimeZone],
                                               _children[_kStartOfWeek]})) {
        // Everything is a constant, so we can turn into a constant.
        return ExpressionConstant::create(
            getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
    }
    if (ExpressionConstant::isConstant(_children[_kUnit])) {
        const Value unitValue =
            _children[_kUnit]->evaluate(Document{}, &(getExpressionContext()->variables));
        if (unitValue.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        _parsedUnit = exec::expression::parseTimeUnit(unitValue, "$dateTrunc"_sd);
    }
    if (ExpressionConstant::isConstant(_children[_kStartOfWeek])) {
        const Value startOfWeekValue =
            _children[_kStartOfWeek]->evaluate(Document{}, &(getExpressionContext()->variables));
        if (startOfWeekValue.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        _parsedStartOfWeek =
            exec::expression::parseDayOfWeek(startOfWeekValue, "$dateTrunc"_sd, "startOfWeek"_sd);
    }
    if (ExpressionConstant::isNullOrConstant(_children[_kTimeZone])) {
        _parsedTimeZone = exec::expression::addContextToAssertionException(
            [&]() {
                return exec::expression::makeTimeZone(getExpressionContext()->getTimeZoneDatabase(),
                                                      Document{},
                                                      _children[_kTimeZone].get(),
                                                      &(getExpressionContext()->variables));
            },
            "$dateTrunc parameter 'timezone' value parsing failed"_sd);
        if (!_parsedTimeZone) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
    }
    if (ExpressionConstant::isConstant(_children[_kBinSize])) {
        const Value binSizeValue =
            _children[_kBinSize]->evaluate(Document{}, &(getExpressionContext()->variables));
        if (binSizeValue.nullish()) {
            return ExpressionConstant::create(getExpressionContext(), Value(BSONNULL));
        }
        _parsedBinSize = exec::expression::convertDateTruncBinSizeValue(binSizeValue);
    }
    return this;
};

Value ExpressionDateTrunc::serialize(const SerializationOptions& options) const {
    return Value{Document{
        {"$dateTrunc"_sd,
         Document{{"date"_sd, _children[_kDate]->serialize(options)},
                  {"unit"_sd, _children[_kUnit]->serialize(options)},
                  {"binSize"_sd,
                   _children[_kBinSize] ? _children[_kBinSize]->serialize(options) : Value{}},
                  {"timezone"_sd,
                   _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value{}},
                  {"startOfWeek"_sd,
                   _children[_kStartOfWeek] ? _children[_kStartOfWeek]->serialize(options)
                                            : Value{}}}}}};
};

Value ExpressionDateTrunc::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

monotonic::State ExpressionDateTrunc::getMonotonicState(const FieldPath& sortedFieldPath) const {
    if (!ExpressionConstant::allNullOrConstant({_children[_kUnit],
                                                _children[_kBinSize],
                                                _children[_kTimeZone],
                                                _children[_kStartOfWeek]})) {
        return monotonic::State::NonMonotonic;
    }
    return _children[_kDate]->getMonotonicState(sortedFieldPath);
}

/* -------------------------- ExpressionGetField ------------------------------ */

REGISTER_STABLE_EXPRESSION(getField, ExpressionGetField::parse);

intrusive_ptr<Expression> ExpressionGetField::parse(ExpressionContext* const expCtx,
                                                    BSONElement expr,
                                                    const VariablesParseState& vps) {
    boost::intrusive_ptr<Expression> fieldExpr;
    boost::intrusive_ptr<Expression> inputExpr;

    if (expr.type() == BSONType::Object) {
        for (auto&& elem : expr.embeddedObject()) {
            const auto fieldName = elem.fieldNameStringData();
            if (!fieldExpr && !inputExpr && fieldName[0] == '$') {
                // This may be an expression, so we should treat it as such.
                fieldExpr = Expression::parseOperand(expCtx, expr, vps);
                inputExpr = ExpressionFieldPath::parse(expCtx, "$$CURRENT", vps);
                break;
            } else if (fieldName == "field"_sd) {
                fieldExpr = Expression::parseOperand(expCtx, elem, vps);
            } else if (fieldName == "input"_sd) {
                inputExpr = Expression::parseOperand(expCtx, elem, vps);
            } else {
                uasserted(3041701,
                          str::stream()
                              << kExpressionName << " found an unknown argument: " << fieldName);
            }
        }
    } else {
        fieldExpr = Expression::parseOperand(expCtx, expr, vps);
        inputExpr = ExpressionFieldPath::parse(expCtx, "$$CURRENT", vps);
    }

    uassert(3041702,
            str::stream() << kExpressionName << " requires 'field' to be specified",
            fieldExpr);
    uassert(3041703,
            str::stream() << kExpressionName << " requires 'input' to be specified",
            inputExpr);

    return make_intrusive<ExpressionGetField>(expCtx, fieldExpr, inputExpr);
}

Value ExpressionGetField::evaluate(const Document& root, Variables* variables) const {
    auto fieldValue = _children[_kField]->evaluate(root, variables);
    // If '_children[_kField]' is a constant expression, the parser guarantees that it evaluates to
    // a string. If it's a dynamic expression, its type can't be deduced during parsing.
    uassert(3041704,
            str::stream() << kExpressionName
                          << " requires 'field' to evaluate to type String, "
                             "but got "
                          << typeName(fieldValue.getType()),
            fieldValue.getType() == BSONType::String);

    auto inputValue = _children[_kInput]->evaluate(root, variables);
    if (inputValue.nullish()) {
        if (inputValue.missing()) {
            return Value();
        } else {
            return Value(BSONNULL);
        }
    } else if (inputValue.getType() != BSONType::Object) {
        return Value();
    }

    return inputValue.getDocument().getField(fieldValue.getString());
}

intrusive_ptr<Expression> ExpressionGetField::optimize() {
    return intrusive_ptr<Expression>(this);
}

Value ExpressionGetField::serialize(const SerializationOptions& options) const {
    Value fieldValue;

    if (auto fieldExprConst = dynamic_cast<ExpressionConstant*>(_children[_kField].get());
        fieldExprConst && fieldExprConst->getValue().getType() == BSONType::String) {
        auto strPath = fieldExprConst->getValue().getString();

        Value maybeRedactedPath{options.serializeFieldPathFromString(strPath)};
        // This is a pretty unique option to serialize. It is both a constant and a field path,
        // which means that it:
        //  - should be redacted (if that option is set).
        //  - should *not* be wrapped in $const iff we are serializing for a debug string
        // However, if we are serializing for a debug string and the string looks like a field
        // reference, it should be wrapped in $const to make it unambiguous with actual field
        // references.
        if (options.literalPolicy != LiteralSerializationPolicy::kToDebugTypeString ||
            strPath[0] == '$') {
            maybeRedactedPath = Value(Document{{"$const"_sd, maybeRedactedPath}});
        }
        fieldValue = maybeRedactedPath;
    } else {
        fieldValue = _children[_kField]->serialize(options);
    }

    return Value(Document{{"$getField"_sd,
                           Document{{"field"_sd, std::move(fieldValue)},
                                    {"input"_sd, _children[_kInput]->serialize(options)}}}});
}

/* -------------------------- ExpressionSetField ------------------------------ */

REGISTER_STABLE_EXPRESSION(setField, ExpressionSetField::parse);

// $unsetField is syntactic sugar for $setField where value is set to $$REMOVE.
REGISTER_STABLE_EXPRESSION(unsetField, ExpressionSetField::parse);

intrusive_ptr<Expression> ExpressionSetField::parse(ExpressionContext* const expCtx,
                                                    BSONElement expr,
                                                    const VariablesParseState& vps) {
    const auto name = expr.fieldNameStringData();
    const bool isUnsetField = name == "$unsetField";

    uassert(4161100,
            str::stream() << name << " only supports an object as its argument",
            expr.type() == BSONType::Object);

    boost::intrusive_ptr<Expression> fieldExpr;
    boost::intrusive_ptr<Expression> inputExpr;
    boost::intrusive_ptr<Expression> valueExpr;

    for (auto&& elem : expr.embeddedObject()) {
        const auto fieldName = elem.fieldNameStringData();
        if (fieldName == "field"_sd) {
            fieldExpr = Expression::parseOperand(expCtx, elem, vps);
        } else if (fieldName == "input"_sd) {
            inputExpr = Expression::parseOperand(expCtx, elem, vps);
        } else if (!isUnsetField && fieldName == "value"_sd) {
            valueExpr = Expression::parseOperand(expCtx, elem, vps);
        } else {
            uasserted(4161101,
                      str::stream() << name << " found an unknown argument: " << fieldName);
        }
    }

    if (isUnsetField) {
        tassert(
            4161110, str::stream() << name << " expects 'value' not to be specified.", !valueExpr);
        valueExpr = ExpressionFieldPath::parse(expCtx, "$$REMOVE", vps);
    }

    uassert(4161102, str::stream() << name << " requires 'field' to be specified", fieldExpr);
    uassert(4161103, str::stream() << name << " requires 'value' to be specified", valueExpr);
    uassert(4161109, str::stream() << name << " requires 'input' to be specified", inputExpr);

    return make_intrusive<ExpressionSetField>(
        expCtx, std::move(fieldExpr), std::move(inputExpr), std::move(valueExpr));
}

Value ExpressionSetField::evaluate(const Document& root, Variables* variables) const {
    auto input = _children[_kInput]->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(4161105,
            str::stream() << kExpressionName << " requires 'input' to evaluate to type Object",
            input.getType() == BSONType::Object);

    auto value = _children[_kValue]->evaluate(root, variables);

    // Build output document and modify 'field'.
    MutableDocument outputDoc(input.getDocument());
    outputDoc.setField(_fieldName, value);
    return outputDoc.freezeToValue();
}

intrusive_ptr<Expression> ExpressionSetField::optimize() {
    return intrusive_ptr<Expression>(this);
}

Value ExpressionSetField::serialize(const SerializationOptions& options) const {
    // The parser guarantees that the '_children[_kField]' expression evaluates to a constant
    // string.
    auto strPath =
        static_cast<ExpressionConstant*>(_children[_kField].get())->getValue().getString();

    Value maybeRedactedPath{options.serializeFieldPathFromString(strPath)};
    // This is a pretty unique option to serialize. It is both a constant and a field path, which
    // means that it:
    //  - should be redacted (if that option is set).
    //  - should *not* be wrapped in $const iff we are serializing for a debug string
    if (options.literalPolicy != LiteralSerializationPolicy::kToDebugTypeString) {
        maybeRedactedPath = Value(Document{{"$const"_sd, maybeRedactedPath}});
    }

    return Value(Document{{"$setField"_sd,
                           Document{{"field"_sd, std::move(maybeRedactedPath)},
                                    {"input"_sd, _children[_kInput]->serialize(options)},
                                    {"value"_sd, _children[_kValue]->serialize(options)}}}});
}

std::string ExpressionSetField::getValidFieldName(boost::intrusive_ptr<Expression> fieldExpr) {
    tassert(9534701,
            str::stream() << kExpressionName << " requires 'field' to be specified",
            fieldExpr);

    // The 'field' argument to '$setField' must evaluate to a constant string, for example,
    // {$const: "$a.b"}. In case the user has forgotten to wrap the value into a '$const' or
    // '$literal' expression, we will raise an error with a more meaningful description.
    if (auto fieldPathExpr = dynamic_cast<ExpressionFieldPath*>(fieldExpr.get()); fieldPathExpr) {
        auto fp = fieldPathExpr->getFieldPath().fullPathWithPrefix();
        uasserted(4161108,
                  str::stream() << "'" << fp
                                << "' is a field path reference which is not allowed "
                                   "in this context. Did you mean {$literal: '"
                                << fp << "'}?");
    }

    auto constFieldExpr = dynamic_cast<ExpressionConstant*>(fieldExpr.get());
    uassert(4161106,
            str::stream() << kExpressionName
                          << " requires 'field' to evaluate to a constant, "
                             "but got a non-constant argument",
            constFieldExpr);
    uassert(4161107,
            str::stream() << kExpressionName
                          << " requires 'field' to evaluate to type String, "
                             "but got "
                          << typeName(constFieldExpr->getValue().getType()),
            constFieldExpr->getValue().getType() == BSONType::String);
    uassert(9534700,
            str::stream() << kExpressionName << ": 'field' cannot contain an embedded null byte",
            constFieldExpr->getValue().getStringData().find('\0') == std::string::npos);

    return constFieldExpr->getValue().getString();
}

/* ------------------------- ExpressionTsSecond ----------------------------- */

Value ExpressionTsSecond::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(tsSecond, ExpressionTsSecond::parse);

/* ------------------------- ExpressionTsIncrement ----------------------------- */

Value ExpressionTsIncrement::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(tsIncrement, ExpressionTsIncrement::parse);

/* ----------------------- ExpressionBitNot ---------------------------- */

Value ExpressionBitNot::evaluateNumericArg(const Value& numericArg) const {
    BSONType type = numericArg.getType();

    if (type == NumberInt) {
        return Value(~numericArg.getInt());
    } else if (type == NumberLong) {
        return Value(~numericArg.getLong());
    } else {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << getOpName()
                                << " only supports int and long, not: " << typeName(type) << ".");
    }
}

REGISTER_STABLE_EXPRESSION(bitNot, ExpressionBitNot::parse);

const char* ExpressionBitNot::getOpName() const {
    return "$bitNot";
}

/* ------------------------- $bitAnd, $bitOr, and $bitXor ------------------------ */

REGISTER_STABLE_EXPRESSION(bitAnd, ExpressionBitAnd::parse);
REGISTER_STABLE_EXPRESSION(bitOr, ExpressionBitOr::parse);
REGISTER_STABLE_EXPRESSION(bitXor, ExpressionBitXor::parse);

Value ExpressionBitAnd::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

Value ExpressionBitOr::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

Value ExpressionBitXor::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

MONGO_INITIALIZER_GROUP(BeginExpressionRegistration, ("default"), ("EndExpressionRegistration"))
MONGO_INITIALIZER_GROUP(EndExpressionRegistration, ("BeginExpressionRegistration"), ())

/* ----------------------- ExpressionInternalKeyStringValue ---------------------------- */

REGISTER_STABLE_EXPRESSION(_internalKeyStringValue, ExpressionInternalKeyStringValue::parse);

boost::intrusive_ptr<Expression> ExpressionInternalKeyStringValue::parse(
    ExpressionContext* expCtx, BSONElement expr, const VariablesParseState& vps) {

    uassert(
        8281500,
        str::stream() << "$_internalKeyStringValue only supports an object as its argument, not "
                      << typeName(expr.type()),
        expr.type() == BSONType::Object);

    boost::intrusive_ptr<Expression> inputExpr;
    boost::intrusive_ptr<Expression> collationExpr;

    for (auto&& element : expr.embeddedObject()) {
        auto field = element.fieldNameStringData();
        if ("input"_sd == field) {
            inputExpr = parseOperand(expCtx, element, vps);
        } else if ("collation"_sd == field) {
            collationExpr = parseOperand(expCtx, element, vps);
        } else {
            uasserted(8281501,
                      str::stream() << "Unrecognized argument to $_internalKeyStringValue: "
                                    << element.fieldName());
        }
    }
    uassert(8281502,
            str::stream() << "$_internalKeyStringValue requires 'input' to be specified",
            inputExpr);

    return make_intrusive<ExpressionInternalKeyStringValue>(expCtx, inputExpr, collationExpr);
}

Value ExpressionInternalKeyStringValue::serialize(const SerializationOptions& options) const {
    return Value(
        Document{{getOpName(),
                  Document{{"input", _children[_kInput]->serialize(options)},
                           {"collation",
                            _children[_kCollation] ? _children[_kCollation]->serialize(options)
                                                   : Value()}}}});
}

Value ExpressionInternalKeyStringValue::evaluate(const Document& root, Variables* variables) const {
    const Value input = _children[_kInput]->evaluate(root, variables);
    auto inputBson = input.wrap("");

    std::unique_ptr<CollatorInterface> collator = nullptr;
    if (_children[_kCollation]) {
        const Value collation = _children[_kCollation]->evaluate(root, variables);
        uassert(8281503,
                str::stream() << "Collation spec must be an object, not "
                              << typeName(collation.getType()),
                collation.isObject());
        auto collationBson = collation.getDocument().toBson();

        auto collatorFactory = CollatorFactoryInterface::get(
            getExpressionContext()->getOperationContext()->getServiceContext());
        collator = uassertStatusOKWithContext(collatorFactory->makeFromBSON(collationBson),
                                              "Invalid collation spec");
    }

    key_string::HeapBuilder ksBuilder(key_string::Version::V1);
    if (collator) {
        ksBuilder.appendBSONElement(inputBson.firstElement(), [&](StringData str) {
            return collator->getComparisonString(str);
        });
    } else {
        ksBuilder.appendBSONElement(inputBson.firstElement());
    }
    auto ksValue = ksBuilder.release();

    // The result omits the typebits so that the numeric value of different types have the same
    // binary representation.
    return Value(
        BSONBinData{ksValue.getBuffer(), static_cast<int>(ksValue.getSize()), BinDataGeneral});
}

/* --------------------------------- Parenthesis --------------------------------------------- */

REGISTER_STABLE_EXPRESSION(expr, parseParenthesisExprObj);
static intrusive_ptr<Expression> parseParenthesisExprObj(ExpressionContext* const expCtx,
                                                         BSONElement bsonExpr,
                                                         const VariablesParseState& vpsIn) {
    return Expression::parseOperand(expCtx, bsonExpr, vpsIn);
}

}  // namespace mongo
