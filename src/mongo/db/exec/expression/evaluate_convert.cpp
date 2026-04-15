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


#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/convert_utils.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/util/str_escape.h"
#include "mongo/util/text.h"

#include <fmt/compile.h>
#include <fmt/format.h>

namespace mongo {

namespace exec::expression {

namespace {

std::string stringifyObjectOrArray(ExpressionContext* expCtx, Value val);

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

    using BaseArg = boost::optional<ConversionBase>;
    using FormatArg = BinDataFormat;
    using SubtypeArg = Value;
    using ByteOrderArg = ConvertByteOrderType;

    using ConversionFunc = ConversionFuncWithExtraArgs<>;
    using ConversionFuncWithBase = ConversionFuncWithExtraArgs<BaseArg>;
    using ConversionFuncWithFormat = ConversionFuncWithExtraArgs<FormatArg>;
    using ConversionFuncWithSubtype = ConversionFuncWithExtraArgs<SubtypeArg>;
    using ConversionFuncWithFormatAndSubtype = ConversionFuncWithExtraArgs<FormatArg, SubtypeArg>;
    using ConversionFuncWithByteOrder = ConversionFuncWithExtraArgs<ByteOrderArg>;
    using ConversionFuncWithByteOrderAndSubtype =
        ConversionFuncWithExtraArgs<ByteOrderArg, SubtypeArg>;

    using AnyConversionFunc = std::variant<std::monostate,
                                           ConversionFunc,
                                           ConversionFuncWithBase,
                                           ConversionFuncWithFormat,
                                           ConversionFuncWithSubtype,
                                           ConversionFuncWithFormatAndSubtype,
                                           ConversionFuncWithByteOrder,
                                           ConversionFuncWithByteOrderAndSubtype>;

    ConversionTable() {
        //
        // Conversions from NumberDouble
        //
        table[stdx::to_underlying(BSONType::numberDouble)]
             [stdx::to_underlying(BSONType::numberDouble)] = &performIdentityConversion;
        table[stdx::to_underlying(BSONType::numberDouble)][stdx::to_underlying(BSONType::string)] =
            &performFormatDouble;
        table[stdx::to_underlying(BSONType::numberDouble)][stdx::to_underlying(BSONType::boolean)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.coerceToBool());
            };
        table[stdx::to_underlying(BSONType::numberDouble)][stdx::to_underlying(BSONType::date)] =
            &performCastNumberToDate;
        table[stdx::to_underlying(BSONType::numberDouble)]
             [stdx::to_underlying(BSONType::numberInt)] = &performCastDoubleToInt;
        table[stdx::to_underlying(BSONType::numberDouble)]
             [stdx::to_underlying(BSONType::numberLong)] = &performCastDoubleToLong;
        table[stdx::to_underlying(BSONType::numberDouble)][stdx::to_underlying(
            BSONType::numberDecimal)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return Value(inputValue.coerceToDecimal());
        };
        table[stdx::to_underlying(BSONType::numberDouble)][stdx::to_underlying(BSONType::binData)] =
            &performConvertDoubleToBinData;

        //
        // Conversions from String
        //
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::numberDouble)] =
            &parseStringToNumber<double, 0>;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::string)] =
            &performIdentityConversion;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::oid)] =
            &parseStringToOID;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::date)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(expCtx->getTimeZoneDatabase()->fromString(
                    inputValue.getStringData(), mongo::TimeZoneDatabase::utcZone()));
            };
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::numberInt)] =
            &parseStringToNumber<int, 10>;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::numberLong)] =
            &parseStringToNumber<long long, 10>;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::numberDecimal)] =
            &parseStringToNumber<Decimal128, 0>;
        table[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::binData)] =
            &parseStringToBinData;

        //
        // Conversions from BinData
        //
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::binData)] =
            &performConvertBinDataToBinData;
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::string)] =
            &performConvertBinDataToString;
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::numberInt)] =
            &performConvertBinDataToInt;
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::numberLong)] =
            &performConvertBinDataToLong;
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::numberDouble)] =
            &performConvertBinDataToDouble;
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::array)] =
            &performConvertBinDataToArray;

        //
        // Conversions from Array
        //
        table[stdx::to_underlying(BSONType::array)][stdx::to_underlying(BSONType::binData)] =
            &performConvertArrayToBinData;

        //
        // Conversions from jstOID
        //
        table[stdx::to_underlying(BSONType::oid)][stdx::to_underlying(BSONType::string)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.getOid().toString());
            };
        table[stdx::to_underlying(BSONType::oid)][stdx::to_underlying(BSONType::oid)] =
            &performIdentityConversion;
        table[stdx::to_underlying(BSONType::oid)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::oid)][stdx::to_underlying(BSONType::date)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.getOid().asDateT());
            };

        //
        // Conversions from Bool
        //
        table[stdx::to_underlying(BSONType::boolean)][stdx::to_underlying(BSONType::numberDouble)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(1.0) : Value(0.0);
            };
        table[stdx::to_underlying(BSONType::boolean)][stdx::to_underlying(BSONType::string)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return inputValue.getBool() ? Value("true"_sd) : Value("false"_sd);
            };
        table[stdx::to_underlying(BSONType::boolean)][stdx::to_underlying(BSONType::boolean)] =
            &performIdentityConversion;
        table[stdx::to_underlying(BSONType::boolean)][stdx::to_underlying(BSONType::numberInt)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(int{1}) : Value(int{0});
            };
        table[stdx::to_underlying(BSONType::boolean)][stdx::to_underlying(BSONType::numberLong)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return inputValue.getBool() ? Value(1LL) : Value(0LL);
            };
        table[stdx::to_underlying(BSONType::boolean)][stdx::to_underlying(
            BSONType::numberDecimal)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return inputValue.getBool() ? Value(Decimal128(1)) : Value(Decimal128(0));
        };

        //
        // Conversions from Date
        //
        table[stdx::to_underlying(BSONType::date)][stdx::to_underlying(BSONType::numberDouble)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(static_cast<double>(inputValue.getDate().toMillisSinceEpoch()));
            };
        table[stdx::to_underlying(BSONType::date)][stdx::to_underlying(BSONType::string)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                auto dateString = uassertStatusOK(TimeZoneDatabase::utcZone().formatDate(
                    kIsoFormatStringZ, inputValue.getDate()));
                return Value(dateString);
            };
        table[stdx::to_underlying(BSONType::date)][stdx::to_underlying(BSONType::boolean)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.coerceToBool());
            };
        table[stdx::to_underlying(BSONType::date)][stdx::to_underlying(BSONType::date)] =
            &performIdentityConversion;
        table[stdx::to_underlying(BSONType::date)][stdx::to_underlying(BSONType::numberLong)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.getDate().toMillisSinceEpoch());
            };
        table[stdx::to_underlying(BSONType::date)][stdx::to_underlying(BSONType::numberDecimal)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(
                    Decimal128(static_cast<int64_t>(inputValue.getDate().toMillisSinceEpoch())));
            };

        //
        // Conversions from bsonTimestamp
        //
        table[stdx::to_underlying(BSONType::timestamp)][stdx::to_underlying(BSONType::date)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.coerceToDate());
            };

        //
        // Conversions from NumberInt
        //
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(
            BSONType::numberDouble)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return Value(inputValue.coerceToDouble());
        };
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(BSONType::string)] =
            &performFormatInt;
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(BSONType::boolean)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.coerceToBool());
            };
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(BSONType::numberInt)] =
            &performIdentityConversion;
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(BSONType::numberLong)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(static_cast<long long>(inputValue.getInt()));
            };
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(
            BSONType::numberDecimal)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return Value(inputValue.coerceToDecimal());
        };
        table[stdx::to_underlying(BSONType::numberInt)][stdx::to_underlying(BSONType::binData)] =
            &performConvertIntToBinData;

        //
        // Conversions from NumberLong
        //
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(
            BSONType::numberDouble)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return Value(inputValue.coerceToDouble());
        };
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(BSONType::string)] =
            &performFormatLong;
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(BSONType::boolean)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return Value(inputValue.coerceToBool());
            };
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(BSONType::date)] =
            &performCastNumberToDate;
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(BSONType::numberInt)] =
            &performCastLongToInt;
        table[stdx::to_underlying(BSONType::numberLong)]
             [stdx::to_underlying(BSONType::numberLong)] = &performIdentityConversion;
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(
            BSONType::numberDecimal)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return Value(inputValue.coerceToDecimal());
        };
        table[stdx::to_underlying(BSONType::numberLong)][stdx::to_underlying(BSONType::binData)] =
            &performConvertLongToBinData;

        //
        // Conversions from NumberDecimal
        //
        table[stdx::to_underlying(BSONType::numberDecimal)]
             [stdx::to_underlying(BSONType::numberDouble)] = &performCastDecimalToDouble;
        table[stdx::to_underlying(BSONType::numberDecimal)][stdx::to_underlying(BSONType::string)] =
            &performFormatDecimal;
        table[stdx::to_underlying(BSONType::numberDecimal)][stdx::to_underlying(
            BSONType::boolean)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return Value(inputValue.coerceToBool());
        };
        table[stdx::to_underlying(BSONType::numberDecimal)][stdx::to_underlying(BSONType::date)] =
            &performCastNumberToDate;
        table[stdx::to_underlying(BSONType::numberDecimal)][stdx::to_underlying(
            BSONType::numberInt)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return performCastDecimalToInt(BSONType::numberInt, inputValue);
        };
        table[stdx::to_underlying(BSONType::numberDecimal)][stdx::to_underlying(
            BSONType::numberLong)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return performCastDecimalToInt(BSONType::numberLong, inputValue);
        };
        table[stdx::to_underlying(BSONType::numberDecimal)]
             [stdx::to_underlying(BSONType::numberDecimal)] = &performIdentityConversion;

        //
        // Miscellaneous conversions to Bool
        //
        table[stdx::to_underlying(BSONType::object)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::array)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::binData)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::regEx)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::dbRef)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::code)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::symbol)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::codeWScope)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;
        table[stdx::to_underlying(BSONType::timestamp)][stdx::to_underlying(BSONType::boolean)] =
            &performConvertToTrue;

        //
        // Any remaining type to String
        //
        fcvGatedTable[stdx::to_underlying(BSONType::regEx)][stdx::to_underlying(BSONType::string)] =
            &performConvertToString;
        fcvGatedTable[stdx::to_underlying(BSONType::undefined)]
                     [stdx::to_underlying(BSONType::string)] = &performConvertToString;
        fcvGatedTable[stdx::to_underlying(BSONType::timestamp)]
                     [stdx::to_underlying(BSONType::string)] = &performConvertToString;
        fcvGatedTable[stdx::to_underlying(BSONType::dbRef)][stdx::to_underlying(BSONType::string)] =
            &performConvertToString;
        fcvGatedTable[stdx::to_underlying(BSONType::codeWScope)]
                     [stdx::to_underlying(BSONType::string)] = &performConvertToString;
        fcvGatedTable[stdx::to_underlying(BSONType::code)][stdx::to_underlying(BSONType::string)] =
            [](ExpressionContext* const, Value inputValue) {
                return Value(inputValue.getCode());
            };
        fcvGatedTable[stdx::to_underlying(BSONType::symbol)][stdx::to_underlying(
            BSONType::string)] = [](ExpressionContext* const, Value inputValue) {
            return Value(inputValue.getSymbol());
        };
        fcvGatedTable[stdx::to_underlying(BSONType::array)][stdx::to_underlying(BSONType::string)] =
            [](ExpressionContext* const expCtx, Value val) {
                return Value(stringifyObjectOrArray(expCtx, std::move(val)));
            };
        fcvGatedTable[stdx::to_underlying(BSONType::object)][stdx::to_underlying(
            BSONType::string)] = [](ExpressionContext* const expCtx, Value val) {
            return Value(stringifyObjectOrArray(expCtx, std::move(val)));
        };

        //
        // Object to BinData
        //
        table[stdx::to_underlying(BSONType::object)][stdx::to_underlying(BSONType::binData)] =
            &performConvertObjectToBinData;

        //
        // String to object/array
        //
        fcvGatedTable[stdx::to_underlying(BSONType::string)][stdx::to_underlying(BSONType::array)] =
            [](ExpressionContext* const expCtx, Value inputValue) {
                return convert_utils::parseJson(inputValue.getStringData(), BSONType::array);
            };
        fcvGatedTable[stdx::to_underlying(BSONType::string)][stdx::to_underlying(
            BSONType::object)] = [](ExpressionContext* const expCtx, Value inputValue) {
            return convert_utils::parseJson(inputValue.getStringData(), BSONType::object);
        };
    }

    ConversionFunc findConversionFunc(BSONType inputType,
                                      BSONType targetType,
                                      boost::optional<ConversionBase> base,
                                      boost::optional<FormatArg> format,
                                      SubtypeArg subtype,
                                      boost::optional<ByteOrderArg> byteOrder,
                                      const bool featureFlagMqlJsEngineGapEnabled) const {
        AnyConversionFunc foundFunction;

        // Note: We can't use BSONType::minKey (-1) or BSONType::maxKey (127) as table indexes,
        // so we have to treat them as special cases.
        if (inputType != BSONType::minKey && inputType != BSONType::maxKey &&
            targetType != BSONType::minKey && targetType != BSONType::maxKey) {
            const int inputT = stdx::to_underlying(inputType);
            const int targetT = stdx::to_underlying(targetType);
            tassert(4607900,
                    str::stream() << "Unexpected input type: " << stdx::to_underlying(inputType),
                    inputT >= 0 && inputT <= stdx::to_underlying(BSONType::jsTypeMax));
            tassert(4607901,
                    str::stream() << "Unexpected target type: " << stdx::to_underlying(targetType),
                    targetT >= 0 && targetT <= stdx::to_underlying(BSONType::jsTypeMax));
            foundFunction = table[inputT][targetT];
            if (featureFlagMqlJsEngineGapEnabled &&
                std::holds_alternative<std::monostate>(foundFunction)) {
                foundFunction = fcvGatedTable[inputT][targetT];
            }
        } else if (targetType == BSONType::boolean) {
            // This is a conversion from MinKey or MaxKey to Bool, which is allowed (and always
            // returns true).
            foundFunction = &performConvertToTrue;
        } else if (featureFlagMqlJsEngineGapEnabled && targetType == BSONType::string) {
            // Similarly, Min/MaxKey to String is also allowed.
            foundFunction = &performConvertToString;
        } else {
            // Any other conversions involving MinKey or MaxKey (either as the target or input) are
            // illegal.
        }

        return makeConversionFunc(foundFunction,
                                  inputType,
                                  targetType,
                                  std::move(base),
                                  std::move(format),
                                  std::move(subtype),
                                  std::move(byteOrder));
    }

private:
    static constexpr int kTableSize = stdx::to_underlying(BSONType::jsTypeMax) + 1;
    AnyConversionFunc table[kTableSize][kTableSize];
    // For conversions that are gated behind featureFlagMqlJsEngineGap.
    AnyConversionFunc fcvGatedTable[kTableSize][kTableSize];

    ConversionFunc makeConversionFunc(AnyConversionFunc foundFunction,
                                      BSONType inputType,
                                      BSONType targetType,
                                      boost::optional<ConversionBase> base,
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
                         [&](ConversionFuncWithBase conversionFunc) {
                             return makeConvertWithExtraArgs(conversionFunc, std::move(base));
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

    static Value performConvertToString(ExpressionContext* const, Value inputValue) {
        return Value(inputValue.toString());
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
        tassert(
            11103502,
            fmt::format("Expected targetType to be either numberInt or numberLong, but found {}",
                        typeName(targetType)),
            targetType == BSONType::numberInt || targetType == BSONType::numberLong);
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
        if (targetType == BSONType::numberInt) {
            int intVal =
                inputDecimal.toInt(&signalingFlags, Decimal128::RoundingMode::kRoundTowardZero);
            result = Value(intVal);
        } else if (targetType == BSONType::numberLong) {
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
        tassert(11103503,
                "Expected signalingFlags to be kNoFlag",
                signalingFlags == Decimal128::SignalingFlag::kNoFlag);

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
            case BSONType::numberLong:
                millisSinceEpoch = inputValue.getLong();
                break;
            case BSONType::numberDouble:
                millisSinceEpoch = performCastDoubleToLong(expCtx, inputValue).getLong();
                break;
            case BSONType::numberDecimal:
                millisSinceEpoch =
                    performCastDecimalToInt(BSONType::numberLong, inputValue).getLong();
                break;
            default:
                MONGO_UNREACHABLE;
        }

        return Value(Date_t::fromMillisSinceEpoch(millisSinceEpoch));
    }

    static Value performFormatNumberWithBase(ExpressionContext* const expCtx,
                                             long long inputValue,
                                             ConversionBase base) {
        switch (base) {
            case mongo::ConversionBase::kBinary:
                return Value(fmt::format("{:b}", inputValue));
            case mongo::ConversionBase::kOctal:
                return Value(fmt::format("{:o}", inputValue));
            case mongo::ConversionBase::kHexadecimal:
                return Value(fmt::format("{:X}", inputValue));
            case mongo::ConversionBase::kDecimal:
                return Value(fmt::format("{:d}", inputValue));
            default:
                MONGO_UNREACHABLE_TASSERT(3501302);
        }
    }

    static Value performFormatDouble(ExpressionContext* const expCtx,
                                     Value inputValue,
                                     boost::optional<ConversionBase> base) {
        double doubleValue = inputValue.getDouble();
        if (!base) {
            if (std::isinf(doubleValue)) {
                return Value(std::signbit(doubleValue) ? "-Infinity"_sd : "Infinity"_sd);
            } else if (std::isnan(doubleValue)) {
                return Value("NaN"_sd);
            } else if (doubleValue == 0.0 && std::signbit(doubleValue)) {
                return Value("-0"_sd);
            } else {
                str::stream str;
                str << fmt::format("{}", doubleValue);
                return Value(StringData(str));
            }
        }

        uassert(
            ErrorCodes::ConversionFailure,
            str::stream() << "Base conversion is not supported for non-integers in $convert with "
                             "no onError value: ",
            inputValue.integral());
        try {
            return performFormatNumberWithBase(
                expCtx, boost::numeric_cast<long long>(doubleValue), *base);

        } catch (boost::bad_numeric_cast&) {
            uasserted(ErrorCodes::ConversionFailure,
                      str::stream()
                          << "Magnitude of the number provided for base conversion is too "
                             "large in $convert with no onError value: ");
        }
    }

    static Value performFormatInt(ExpressionContext* const expCtx,
                                  Value inputValue,
                                  boost::optional<ConversionBase> base) {
        int intValue = inputValue.getInt();
        if (!base)
            return Value(StringData(str::stream() << intValue));
        return performFormatNumberWithBase(expCtx, intValue, *base);
    }

    static Value performFormatLong(ExpressionContext* const expCtx,
                                   Value inputValue,
                                   boost::optional<ConversionBase> base) {
        long long longValue = inputValue.getLong();
        if (!base)
            return Value(StringData(str::stream() << longValue));
        return performFormatNumberWithBase(expCtx, longValue, *base);
    }

    static Value performFormatDecimal(ExpressionContext* const expCtx,
                                      Value inputValue,
                                      boost::optional<ConversionBase> base) {
        Decimal128 decimalValue = inputValue.getDecimal();
        if (!base)
            return Value(decimalValue.toString());

        uassert(
            ErrorCodes::ConversionFailure,
            str::stream() << "Base conversion is not supported for non-integers in $convert with "
                             "no onError value: ",
            inputValue.integral());
        uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
        long long longValue = decimalValue.toLong(&signalingFlags);
        if (signalingFlags == Decimal128::SignalingFlag::kNoFlag) {
            return performFormatNumberWithBase(expCtx, longValue, *base);
        }
        uasserted(ErrorCodes::ConversionFailure,
                  str::stream() << "Magnitude of the number provided for base conversion is too "
                                   "large in $convert with no onError value: ");
    }

    template <class targetType, int defaultBase>
    static Value parseStringToNumber(ExpressionContext* const expCtx,
                                     Value inputValue,
                                     boost::optional<ConversionBase> convBase) {
        auto stringValue = inputValue.getStringData();
        // Reject any strings in hex format. This check is needed because the
        // NumberParser::parse call below allows an input hex string prefixed by '0x' when
        // parsing to a double.
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Illegal hexadecimal input in $convert with no onError value: "
                              << stringValue,
                !stringValue.starts_with("0x") && !stringValue.starts_with("0X"));
        if (!convBase) {
            targetType result;
            Status parseStatus = NumberParser().base(defaultBase)(stringValue, &result);
            uassert(ErrorCodes::ConversionFailure,
                    str::stream() << "Failed to parse number '" << stringValue
                                  << "' in $convert with no onError value: "
                                  << parseStatus.reason(),
                    parseStatus.isOK());
            return Value(result);
        }
        // NumberParser needs base 0 when converting to double or decimal, so when converting with a
        // specified base, we first convert to long long and then convert the long long to the
        // target type.
        long long parseResult;
        Status parseStatus =
            NumberParser().base(static_cast<int>(*convBase))(stringValue, &parseResult);
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Failed to parse number '" << stringValue
                              << "' in $convert with no onError value: " << parseStatus.reason(),
                parseStatus.isOK());
        // Convert long long into correct target type.
        if constexpr (std::is_same_v<targetType, int>)
            return performCastLongToInt(expCtx, Value(parseResult));
        else if constexpr (std::is_same_v<targetType, double>)
            return Value(static_cast<double>(parseResult));
        else if constexpr (std::is_same_v<targetType, Decimal128>)
            return Value(Decimal128(parseResult));
        else
            return Value(parseResult);
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

                    auto decoded = std::string{input};
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

    using dType = convert_utils::dType;

    static std::string toString(dType v) {
        switch (v) {
            case dType::PACKED_BIT:
                return "PACKED_BIT";
            case dType::INT8:
                return "INT8";
            case dType::FLOAT32:
                return "FLOAT32";
        }
        MONGO_UNREACHABLE_TASSERT(10506611);
    }

    // Determines the minimum dType that the Value argument can be represented as.
    static dType getSupportedDType(Value v) {
        if (v.getType() == BSONType::boolean) {
            return dType::PACKED_BIT;
        }
        if (v.numeric()) {
            if (v.getType() == BSONType::numberInt) {
                auto num = v.getInt();
                if (num == 0 || num == 1) {
                    return dType::PACKED_BIT;
                }
                if (num >= -128 && num <= 127) {
                    return dType::INT8;
                } else {
                    // TODO SERVER-106059 Support this case by introducing new type, or implicitly
                    // converting to float32.
                    uasserted(
                        ErrorCodes::ConversionFailure,
                        "Converting a BSON array with integers larger than INT8 is not supported.");
                }
            }
            if (v.getType() == BSONType::numberDouble) {
                if (v.integral()) {
                    auto vint = v.coerceToInt();
                    if (vint <= std::numeric_limits<int8_t>::max() &&
                        vint >= std::numeric_limits<int8_t>::min()) {
                        return dType::INT8;
                    }
                }

                return dType::FLOAT32;
            }
        }
        uasserted(ErrorCodes::ConversionFailure,
                  "Converting array to BinData requires an array with numeric or bool content");
    }

    static std::byte getByteForDType(dType t) {
        switch (t) {
            case dType::PACKED_BIT:
                return convert_utils::kPackedBitDataTypeByte;
            case dType::INT8:
                return convert_utils::kInt8DataTypeByte;
            case dType::FLOAT32:
                return convert_utils::kFloat32DataTypeByte;
        }
        MONGO_UNREACHABLE_TASSERT(10506604);
    }

    static size_t bitsRequiredForDType(int numElements, dType t) {
        uassert(10506610,
                str::stream() << "Conversion of " << std::to_string(numElements) << " elements to "
                              << toString(t) << " bindata vector might overflow.",
                (size_t)numElements <= (std::numeric_limits<size_t>::max() / 32));
        switch (t) {
            case dType::PACKED_BIT:
                return numElements;
            case dType::INT8:
                return numElements * 8;
            case dType::FLOAT32:
                return numElements * 32;
        }
        MONGO_UNREACHABLE_TASSERT(10506605);
    }

    static Value performConvertBinDataToArray(ExpressionContext* const expCtx,
                                              Value inputValue,
                                              ByteOrderArg byteOrder,
                                              SubtypeArg subtypeValue) {
        if (!feature_flags::gFeatureFlagConvertBinDataVectors.isEnabled(
                VersionContext::getDecoration(expCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            uasserted(10506607, "$convert from BinData vector to BSON array is not enabled");
        }

        auto binData = inputValue.getBinData();
        uassert(ErrorCodes::ConversionFailure,
                "Conversion from binData to array is only supported for bindata with the vector "
                "subtype",
                binData.type == BinDataType::Vector);

        bool isLittleEndian = (byteOrder == ConvertByteOrderType::little);
        return Value(convert_utils::convertBinDataVectorToArray(inputValue, isLittleEndian));
    }

    static Value performConvertArrayToBinData(ExpressionContext* const expCtx,
                                              Value inputValue,
                                              ByteOrderArg byteOrder,
                                              SubtypeArg subtypeValue) {
        if (!feature_flags::gFeatureFlagConvertBinDataVectors.isEnabled(
                VersionContext::getDecoration(expCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            uasserted(10506608, "$convert from BSON array to BinData vector is not enabled");
        }

        uassert(ErrorCodes::ConversionFailure,
                "Converting array to BinData requires array",
                inputValue.isArray());
        auto arr = inputValue.getArray();
        // Scan the array to pick a dtype value.
        // PackedBit can be used if all values are 0 or 1.
        dType currentType = dType::PACKED_BIT;
        for (const auto& obj : arr) {
            // TODO SERVER-105750 Determine what type to use, possibly from user input.
            currentType = std::max(currentType, getSupportedDType(obj));
        }

        // We know everything can be encoded in our selected format.
        // Create a data array.
        std::vector<std::byte> byteArray;

        // First byte is dType.
        byteArray.push_back(getByteForDType(currentType));

        // Second byte is padding.
        // Padding is equal to the number of bits required to finish the last byte.
        // Total bytes required is numElements * elementSize
        const auto bitsRequired = bitsRequiredForDType(arr.size(), currentType);
        const auto extraBits = bitsRequired % 8;
        const int padding = extraBits == 0 ? 0 : 8 - (bitsRequired % 8);
        byteArray.push_back(std::byte(padding));

        // Variables for tracking the PACKED_BIT situation.
        int bitsInByteUsed = 0;
        std::uint8_t currentByte = 0;
        for (const auto& obj : arr) {
            switch (currentType) {
                case dType::PACKED_BIT:
                    if (obj.coerceToBool()) {
                        currentByte |= (1 << (7 - bitsInByteUsed));
                        // If it is false, no need for action because the byte is already 0'd out at
                        // the beginning.
                    }
                    ++bitsInByteUsed;
                    if (bitsInByteUsed == 8) {
                        byteArray.push_back(std::byte(currentByte));
                        bitsInByteUsed = 0;
                        currentByte = 0;
                    }
                    break;
                case dType::INT8:
                    byteArray.push_back(static_cast<std::byte>(obj.coerceToInt()));
                    break;
                case dType::FLOAT32:
                    // Note that casting to a float here truncates the double and may lose
                    // precision.
                    auto value = writeNumberAccordingToEndianness<float>(
                        static_cast<float>(obj.coerceToDouble()), byteOrder, subtypeValue);
                    auto binData = value.getBinData();
                    byteArray.resize(byteArray.size() + sizeof(float));
                    std::memcpy(byteArray.data() + byteArray.size() - sizeof(float),
                                binData.data,
                                sizeof(float));
                    break;
            }
        }
        if (bitsInByteUsed != 0) {
            byteArray.push_back(std::byte(currentByte));
            auto actualPadding = 8 - bitsInByteUsed;
            tassert(10506603,
                    "Mismatched padding after serializing array to binData vector",
                    actualPadding == padding);
        }
        auto thisBinData = BSONBinData(byteArray.data(), byteArray.size(), BinDataType::Vector);
        // Note that the Value internals copy the data inside of the binData vector into a new
        // StringData so we do not have to worry about ownership semantics here.
        return Value(std::move(thisBinData));
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
        }
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

    static Value performConvertObjectToBinData(ExpressionContext* const expCtx,
                                               Value inputValue,
                                               SubtypeArg subtypeValue) {
        if (!feature_flags::gFeatureFlagConvertObjectToBinData.isEnabled(
                VersionContext::getDecoration(expCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            uasserted(ErrorCodes::ConversionFailure,
                      "$convert from Object to BinData is not enabled");
        }

        auto bsonObj = inputValue.getDocument().toBson();
        auto binDataType = computeBinDataType(subtypeValue);
        return Value(BSONBinData(bsonObj.objdata(), bsonObj.objsize(), binDataType));
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

boost::optional<ConversionBase> parseBase(Value baseValue) {
    if (baseValue.nullish()) {
        return {};
    }

    uassert(3501300,
            str::stream() << "In $convert, 'base' argument is not an integer",
            baseValue.integral());

    int base = baseValue.coerceToInt();

    uassert(3501301,
            str::stream() << "In $convert, 'base' argument is not a valid base",
            isValidConversionBase(base));

    return static_cast<ConversionBase>(base);
}

boost::optional<BinDataFormat> parseBinDataFormat(Value formatValue) {
    if (formatValue.nullish()) {
        return {};
    }

    uassert(4341114,
            str::stream() << "$convert requires that 'format' be a string, found: "
                          << typeName(formatValue.getType()) << " with value "
                          << formatValue.toString(),
            formatValue.getType() == BSONType::string);

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
            byteOrderValue.getType() == BSONType::string);

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

bool requestingConvertBinDataNumeric(ExpressionConvert::ConvertTargetTypeInfo targetTypeInfo,
                                     BSONType inputType) {
    return (inputType == BSONType::binData &&
            (targetTypeInfo.type == BSONType::numberInt ||
             targetTypeInfo.type == BSONType::numberLong ||
             targetTypeInfo.type == BSONType::numberDouble)) ||
        ((inputType == BSONType::numberInt || inputType == BSONType::numberLong ||
          inputType == BSONType::numberDouble) &&
         targetTypeInfo.type == BSONType::binData);
}

Value performConversion(const ExpressionConvert& expr,
                        ExpressionConvert::ConvertTargetTypeInfo targetTypeInfo,
                        Value inputValue,
                        boost::optional<ConversionBase> base,
                        boost::optional<BinDataFormat> format,
                        boost::optional<ConvertByteOrderType> byteOrder) {
    tassert(11103504,
            fmt::format("Expected inputValue to not be nullish, but found {}",
                        typeName(inputValue.getType())),
            !inputValue.nullish());

    static const ConversionTable table;
    BSONType inputType = inputValue.getType();

    uassert(ErrorCodes::ConversionFailure,
            str::stream() << "BinData $convert is not allowed in the current feature "
                             "compatibility version. See "
                          << feature_compatibility_version_documentation::compatibilityLink()
                          << ".",
            expr.getAllowBinDataConvert() || targetTypeInfo.type == BSONType::boolean ||
                (inputType != BSONType::binData && targetTypeInfo.type != BSONType::binData));

    uassert(ErrorCodes::ConversionFailure,
            str::stream()
                << "BinData $convert with numeric values is not allowed in the current feature "
                   "compatibility version. See "
                << feature_compatibility_version_documentation::compatibilityLink() << ".",
            expr.getAllowBinDataConvertNumeric() ||
                !requestingConvertBinDataNumeric(targetTypeInfo, inputType));

    return table.findConversionFunc(
        inputType,
        targetTypeInfo.type,
        base,
        format,
        targetTypeInfo.subtype,
        byteOrder,
        expr.getExpressionContext()->isFeatureFlagMqlJsEngineGapEnabled())(
        expr.getExpressionContext(), inputValue);
}

/**
 * Stringifies BSON objects and arrays to produce a valid JSON string. Types that have a JSON
 * counterpart are not wrapped in strings and can be parsed back to the same value. BSON specific
 * types are written as JSON string literals.
 */
class JsonStringGenerator {
public:
    JsonStringGenerator(ExpressionContext* expCtx, size_t maxSize)
        : _expCtx(expCtx), _maxSize(maxSize) {}

    void writeValue(fmt::memory_buffer& buffer, const Value& val) const {
        switch (val.getType()) {
            // The below types have a JSON counterpart.
            case BSONType::eoo:
            case BSONType::null:
            case BSONType::undefined:
                // Existing behavior in $convert is to treat all nullish values as null.
                appendTo(buffer, "null"_sd);
                break;
            case BSONType::boolean:
                appendTo(buffer, val.getBool() ? "true"_sd : "false"_sd);
                break;
            case BSONType::numberDecimal:
            case BSONType::numberDouble:
            case BSONType::numberLong:
            case BSONType::numberInt:
                writeNumeric(buffer, val);
                break;
            // The below types (except string) don't have a JSON counterpart so we must wrap them in
            // a string.
            case BSONType::minKey:
            case BSONType::maxKey:
            case BSONType::oid:
            case BSONType::binData:
            case BSONType::date:
            case BSONType::timestamp:
                writeUnescapedString(buffer, stringifyValue(val));
                break;
            case BSONType::string:
            case BSONType::regEx:
            case BSONType::symbol:
            case BSONType::codeWScope:
            case BSONType::dbRef:
            case BSONType::code:
                writeEscapedString(buffer, stringifyValue(val));
                break;
            // Nested types.
            case BSONType::object:
                writeObject(buffer, val.getDocument());
                break;
            case BSONType::array:
                writeArray(buffer, val.getArray());
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(4607906);
        }

        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Resulting string exceeds maximum size (" << _maxSize << " bytes)",
                buffer.size() <= _maxSize);
    }

private:
    static void appendTo(fmt::memory_buffer& buffer, StringData data) {
        buffer.append(data.data(), data.data() + data.size());
    }

    static void writeEscapedString(fmt::memory_buffer& buffer, StringData str) {
        buffer.push_back('"');
        str::escapeForJSON(buffer, str);
        buffer.push_back('"');
    }

    static void writeUnescapedString(fmt::memory_buffer& buffer, StringData str) {
        buffer.push_back('"');
        appendTo(buffer, str);
        buffer.push_back('"');
    }

    void writeNumeric(fmt::memory_buffer& buffer, const Value& val) const {
        tassert(4607904, "Expected a number", val.numeric());

        if (val.isNaN() || val.isInfinite()) {
            writeUnescapedString(buffer, stringifyValue(val));
        } else {
            appendTo(buffer, stringifyValue(val));
        }
    }

    void writeObject(fmt::memory_buffer& buffer, const Document& doc) const {
        buffer.push_back('{');
        bool first{true};
        for (auto it = doc.fieldIterator(); it.more();) {
            if (!first) {
                buffer.push_back(',');
            }
            first = false;

            auto&& [key, value] = it.next();
            fmt::format_to(std::back_inserter(buffer), FMT_COMPILE(R"("{}":)"), key);
            writeValue(buffer, value);
        }
        buffer.push_back('}');
    }

    void writeArray(fmt::memory_buffer& buffer, const std::vector<Value>& arr) const {
        buffer.push_back('[');
        for (size_t i = 0; i < arr.size(); i++) {
            if (i > 0) {
                buffer.push_back(',');
            }

            writeValue(buffer, arr[i]);
        }
        buffer.push_back(']');
    }

    /**
     * Stringifies a value as if it was passed as an input to $toString.
     */
    std::string stringifyValue(Value input) const {
        tassert(4607905,
                "Expected a non-nested value",
                input.getType() != BSONType::array && input.getType() != BSONType::object);

        static const ConversionTable table;
        auto conversionFunc =
            table.findConversionFunc(input.getType(),
                                     BSONType::string,
                                     boost::none /*base*/,
                                     BinDataFormat::kAuto,
                                     // Unused.
                                     ExpressionConvert::ConvertTargetTypeInfo::defaultSubtypeVal,
                                     boost::none /*byteOrder*/,
                                     // Conversions using this class are already gated by this FF.
                                     true /*featureFlagMqlJsEngineGapEnabled*/);

        Value result = conversionFunc(_expCtx, input);
        tassert(4607903, "Expected string", result.getType() == BSONType::string);
        return result.getString();
    }

    ExpressionContext* const _expCtx;
    const size_t _maxSize;
};

std::string stringifyObjectOrArray(ExpressionContext* const expCtx, Value val) {
    fmt::memory_buffer buffer;
    JsonStringGenerator generator{expCtx, BSONObjMaxUserSize};
    generator.writeValue(buffer, val);
    return fmt::to_string(buffer);
}

}  // namespace

Value evaluate(const ExpressionConvert& expr, const Document& root, Variables* variables) {
    auto toValue = expr.getTo()->evaluate(root, variables);
    auto inputValue = expr.getInput()->evaluate(root, variables);
    auto baseValue = expr.getBase() ? expr.getBase()->evaluate(root, variables) : Value();
    auto formatValue = expr.getFormat() ? expr.getFormat()->evaluate(root, variables) : Value();
    auto byteOrderValue =
        expr.getByteOrder() ? expr.getByteOrder()->evaluate(root, variables) : Value();

    auto targetTypeInfo = ExpressionConvert::ConvertTargetTypeInfo::parse(toValue);

    if (inputValue.nullish()) {
        return expr.getOnNull() ? expr.getOnNull()->evaluate(root, variables) : Value(BSONNULL);
    }

    if (!targetTypeInfo) {
        // "to" evaluated to a nullish value.
        return Value(BSONNULL);
    }

    auto base = parseBase(baseValue);
    auto format = parseBinDataFormat(formatValue);
    auto byteOrder = parseByteOrder(byteOrderValue);

    try {
        return performConversion(expr, *targetTypeInfo, inputValue, base, format, byteOrder);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (expr.getOnError()) {
            return expr.getOnError()->evaluate(root, variables);
        } else {
            throw;
        }
    }
}

}  // namespace exec::expression
}  // namespace mongo
