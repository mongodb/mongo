/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_SIMD_h
#define builtin_SIMD_h

#include "jsapi.h"
#include "jsobj.h"

#include "builtin/TypedObject.h"
#include "js/Conversions.h"
#include "vm/GlobalObject.h"

/*
 * JS SIMD functions.
 * Spec matching polyfill:
 * https://github.com/johnmccutchan/ecmascript_simd/blob/master/src/ecmascript_simd.js
 */

#define FLOAT32X4_UNARY_FUNCTION_LIST(V)                                              \
  V(abs, (UnaryFunc<Float32x4, Abs, Float32x4>), 1)                                   \
  V(check, (UnaryFunc<Float32x4, Identity, Float32x4>), 1)                            \
  V(fromFloat64x2, (FuncConvert<Float64x2, Float32x4> ), 1)                           \
  V(fromFloat64x2Bits, (FuncConvertBits<Float64x2, Float32x4>), 1)                    \
  V(fromInt8x16Bits, (FuncConvertBits<Int8x16, Float32x4>), 1)                        \
  V(fromInt16x8Bits, (FuncConvertBits<Int16x8, Float32x4>), 1)                        \
  V(fromInt32x4, (FuncConvert<Int32x4, Float32x4> ), 1)                               \
  V(fromInt32x4Bits, (FuncConvertBits<Int32x4, Float32x4>), 1)                        \
  V(neg, (UnaryFunc<Float32x4, Neg, Float32x4>), 1)                                   \
  V(not, (CoercedUnaryFunc<Float32x4, Int32x4, Not, Float32x4>), 1)                   \
  V(reciprocalApproximation, (UnaryFunc<Float32x4, RecApprox, Float32x4>), 1)         \
  V(reciprocalSqrtApproximation, (UnaryFunc<Float32x4, RecSqrtApprox, Float32x4>), 1) \
  V(splat, (FuncSplat<Float32x4>), 1)                                                 \
  V(sqrt, (UnaryFunc<Float32x4, Sqrt, Float32x4>), 1)

#define FLOAT32X4_BINARY_FUNCTION_LIST(V)                                             \
  V(add, (BinaryFunc<Float32x4, Add, Float32x4>), 2)                                  \
  V(and, (CoercedBinaryFunc<Float32x4, Int32x4, And, Float32x4>), 2)                  \
  V(div, (BinaryFunc<Float32x4, Div, Float32x4>), 2)                                  \
  V(equal, (CompareFunc<Float32x4, Equal, Int32x4>), 2)                               \
  V(extractLane, (ExtractLane<Float32x4>), 2)                                         \
  V(greaterThan, (CompareFunc<Float32x4, GreaterThan, Int32x4>), 2)                   \
  V(greaterThanOrEqual, (CompareFunc<Float32x4, GreaterThanOrEqual, Int32x4>), 2)     \
  V(lessThan, (CompareFunc<Float32x4, LessThan, Int32x4>), 2)                         \
  V(lessThanOrEqual, (CompareFunc<Float32x4, LessThanOrEqual, Int32x4>), 2)           \
  V(load,  (Load<Float32x4, 4>), 2)                                                   \
  V(load3, (Load<Float32x4, 3>), 2)                                                   \
  V(load2, (Load<Float32x4, 2>), 2)                                                   \
  V(load1, (Load<Float32x4, 1>), 2)                                                   \
  V(max, (BinaryFunc<Float32x4, Maximum, Float32x4>), 2)                              \
  V(maxNum, (BinaryFunc<Float32x4, MaxNum, Float32x4>), 2)                            \
  V(min, (BinaryFunc<Float32x4, Minimum, Float32x4>), 2)                              \
  V(minNum, (BinaryFunc<Float32x4, MinNum, Float32x4>), 2)                            \
  V(mul, (BinaryFunc<Float32x4, Mul, Float32x4>), 2)                                  \
  V(notEqual, (CompareFunc<Float32x4, NotEqual, Int32x4>), 2)                         \
  V(or, (CoercedBinaryFunc<Float32x4, Int32x4, Or, Float32x4>), 2)                    \
  V(sub, (BinaryFunc<Float32x4, Sub, Float32x4>), 2)                                  \
  V(xor, (CoercedBinaryFunc<Float32x4, Int32x4, Xor, Float32x4>), 2)

#define FLOAT32X4_TERNARY_FUNCTION_LIST(V)                                            \
  V(replaceLane, (ReplaceLane<Float32x4>), 3)                                         \
  V(select, (Select<Float32x4, Int32x4>), 3)                                          \
  V(store,  (Store<Float32x4, 4>), 3)                                                 \
  V(store3, (Store<Float32x4, 3>), 3)                                                 \
  V(store2, (Store<Float32x4, 2>), 3)                                                 \
  V(store1, (Store<Float32x4, 1>), 3)

#define FLOAT32X4_SHUFFLE_FUNCTION_LIST(V)                                            \
  V(swizzle, Swizzle<Float32x4>, 5)                                                   \
  V(shuffle, Shuffle<Float32x4>, 6)

#define FLOAT32X4_FUNCTION_LIST(V)                                                    \
  FLOAT32X4_UNARY_FUNCTION_LIST(V)                                                    \
  FLOAT32X4_BINARY_FUNCTION_LIST(V)                                                   \
  FLOAT32X4_TERNARY_FUNCTION_LIST(V)                                                  \
  FLOAT32X4_SHUFFLE_FUNCTION_LIST(V)

#define FLOAT64X2_UNARY_FUNCTION_LIST(V)                                              \
  V(abs, (UnaryFunc<Float64x2, Abs, Float64x2>), 1)                                   \
  V(check, (UnaryFunc<Float64x2, Identity, Float64x2>), 1)                            \
  V(fromFloat32x4, (FuncConvert<Float32x4, Float64x2> ), 1)                           \
  V(fromFloat32x4Bits, (FuncConvertBits<Float32x4, Float64x2>), 1)                    \
  V(fromInt8x16Bits, (FuncConvertBits<Int8x16, Float64x2>), 1)                        \
  V(fromInt16x8Bits, (FuncConvertBits<Int16x8, Float64x2>), 1)                        \
  V(fromInt32x4, (FuncConvert<Int32x4, Float64x2> ), 1)                               \
  V(fromInt32x4Bits, (FuncConvertBits<Int32x4, Float64x2>), 1)                        \
  V(neg, (UnaryFunc<Float64x2, Neg, Float64x2>), 1)                                   \
  V(reciprocalApproximation, (UnaryFunc<Float64x2, RecApprox, Float64x2>), 1)         \
  V(reciprocalSqrtApproximation, (UnaryFunc<Float64x2, RecSqrtApprox, Float64x2>), 1) \
  V(splat, (FuncSplat<Float64x2>), 1)                                                 \
  V(sqrt, (UnaryFunc<Float64x2, Sqrt, Float64x2>), 1)

#define FLOAT64X2_BINARY_FUNCTION_LIST(V)                                             \
  V(add, (BinaryFunc<Float64x2, Add, Float64x2>), 2)                                  \
  V(div, (BinaryFunc<Float64x2, Div, Float64x2>), 2)                                  \
  V(equal, (CompareFunc<Float64x2, Equal, Int32x4>), 2)                               \
  V(extractLane, (ExtractLane<Float64x2>), 2)                                         \
  V(greaterThan, (CompareFunc<Float64x2, GreaterThan, Int32x4>), 2)                   \
  V(greaterThanOrEqual, (CompareFunc<Float64x2, GreaterThanOrEqual, Int32x4>), 2)     \
  V(lessThan, (CompareFunc<Float64x2, LessThan, Int32x4>), 2)                         \
  V(lessThanOrEqual, (CompareFunc<Float64x2, LessThanOrEqual, Int32x4>), 2)           \
  V(load,  (Load<Float64x2, 2>), 2)                                                   \
  V(load1, (Load<Float64x2, 1>), 2)                                                   \
  V(max, (BinaryFunc<Float64x2, Maximum, Float64x2>), 2)                              \
  V(maxNum, (BinaryFunc<Float64x2, MaxNum, Float64x2>), 2)                            \
  V(min, (BinaryFunc<Float64x2, Minimum, Float64x2>), 2)                              \
  V(minNum, (BinaryFunc<Float64x2, MinNum, Float64x2>), 2)                            \
  V(mul, (BinaryFunc<Float64x2, Mul, Float64x2>), 2)                                  \
  V(notEqual, (CompareFunc<Float64x2, NotEqual, Int32x4>), 2)                         \
  V(sub, (BinaryFunc<Float64x2, Sub, Float64x2>), 2)

#define FLOAT64X2_TERNARY_FUNCTION_LIST(V)                                            \
  V(replaceLane, (ReplaceLane<Float64x2>), 3)                                         \
  V(select, (Select<Float64x2, Int32x4>), 3)                                          \
  V(store,  (Store<Float64x2, 2>), 3)                                                 \
  V(store1, (Store<Float64x2, 1>), 3)

#define FLOAT64X2_SHUFFLE_FUNCTION_LIST(V)                                            \
  V(swizzle, Swizzle<Float64x2>, 3)                                                   \
  V(shuffle, Shuffle<Float64x2>, 4)

#define FLOAT64X2_FUNCTION_LIST(V)                                                    \
  FLOAT64X2_UNARY_FUNCTION_LIST(V)                                                    \
  FLOAT64X2_BINARY_FUNCTION_LIST(V)                                                   \
  FLOAT64X2_TERNARY_FUNCTION_LIST(V)                                                  \
  FLOAT64X2_SHUFFLE_FUNCTION_LIST(V)

#define INT8X16_UNARY_FUNCTION_LIST(V)                                                \
  V(check, (UnaryFunc<Int8x16, Identity, Int8x16>), 1)                                \
  V(fromFloat32x4Bits, (FuncConvertBits<Float32x4, Int8x16>), 1)                      \
  V(fromFloat64x2Bits, (FuncConvertBits<Float64x2, Int8x16>), 1)                      \
  V(fromInt16x8Bits, (FuncConvertBits<Int16x8, Int8x16>), 1)                          \
  V(fromInt32x4Bits, (FuncConvertBits<Int32x4, Int8x16>), 1)                          \
  V(neg, (UnaryFunc<Int8x16, Neg, Int8x16>), 1)                                       \
  V(not, (UnaryFunc<Int8x16, Not, Int8x16>), 1)                                       \
  V(splat, (FuncSplat<Int8x16>), 1)

#define INT8X16_BINARY_FUNCTION_LIST(V)                                               \
  V(add, (BinaryFunc<Int8x16, Add, Int8x16>), 2)                                      \
  V(and, (BinaryFunc<Int8x16, And, Int8x16>), 2)                                      \
  V(equal, (CompareFunc<Int8x16, Equal, Int8x16>), 2)                                 \
  V(extractLane, (ExtractLane<Int8x16>), 2)                                           \
  V(greaterThan, (CompareFunc<Int8x16, GreaterThan, Int8x16>), 2)                     \
  V(greaterThanOrEqual, (CompareFunc<Int8x16, GreaterThanOrEqual, Int8x16>), 2)       \
  V(lessThan, (CompareFunc<Int8x16, LessThan, Int8x16>), 2)                           \
  V(lessThanOrEqual, (CompareFunc<Int8x16, LessThanOrEqual, Int8x16>), 2)             \
  V(load, (Load<Int8x16, 16>), 2)                                                     \
  V(mul, (BinaryFunc<Int8x16, Mul, Int8x16>), 2)                                      \
  V(notEqual, (CompareFunc<Int8x16, NotEqual, Int8x16>), 2)                           \
  V(or, (BinaryFunc<Int8x16, Or, Int8x16>), 2)                                        \
  V(sub, (BinaryFunc<Int8x16, Sub, Int8x16>), 2)                                      \
  V(shiftLeftByScalar, (BinaryScalar<Int8x16, ShiftLeft>), 2)                         \
  V(shiftRightArithmeticByScalar, (BinaryScalar<Int8x16, ShiftRightArithmetic>), 2)   \
  V(shiftRightLogicalByScalar, (BinaryScalar<Int8x16, ShiftRightLogical>), 2)         \
  V(xor, (BinaryFunc<Int8x16, Xor, Int8x16>), 2)

#define INT8X16_TERNARY_FUNCTION_LIST(V)                                              \
  V(replaceLane, (ReplaceLane<Int8x16>), 3)                                           \
  V(select, (Select<Int8x16, Int8x16>), 3)                                            \
  V(selectBits, (SelectBits<Int8x16, Int8x16>), 3)                                            \
  V(store, (Store<Int8x16, 16>), 3)

#define INT8X16_SHUFFLE_FUNCTION_LIST(V)                                              \
  V(swizzle, Swizzle<Int8x16>, 17)                                                    \
  V(shuffle, Shuffle<Int8x16>, 18)

#define INT8X16_FUNCTION_LIST(V)                                                      \
  INT8X16_UNARY_FUNCTION_LIST(V)                                                      \
  INT8X16_BINARY_FUNCTION_LIST(V)                                                     \
  INT8X16_TERNARY_FUNCTION_LIST(V)                                                    \
  INT8X16_SHUFFLE_FUNCTION_LIST(V)

#define INT16X8_UNARY_FUNCTION_LIST(V)                                                \
  V(check, (UnaryFunc<Int16x8, Identity, Int16x8>), 1)                                \
  V(fromFloat32x4Bits, (FuncConvertBits<Float32x4, Int16x8>), 1)                      \
  V(fromFloat64x2Bits, (FuncConvertBits<Float64x2, Int16x8>), 1)                      \
  V(fromInt8x16Bits, (FuncConvertBits<Int8x16, Int16x8>), 1)                          \
  V(fromInt32x4Bits, (FuncConvertBits<Int32x4, Int16x8>), 1)                          \
  V(neg, (UnaryFunc<Int16x8, Neg, Int16x8>), 1)                                       \
  V(not, (UnaryFunc<Int16x8, Not, Int16x8>), 1)                                       \
  V(splat, (FuncSplat<Int16x8>), 1)

#define INT16X8_BINARY_FUNCTION_LIST(V)                                               \
  V(add, (BinaryFunc<Int16x8, Add, Int16x8>), 2)                                      \
  V(and, (BinaryFunc<Int16x8, And, Int16x8>), 2)                                      \
  V(equal, (CompareFunc<Int16x8, Equal, Int16x8>), 2)                                 \
  V(extractLane, (ExtractLane<Int16x8>), 2)                                           \
  V(greaterThan, (CompareFunc<Int16x8, GreaterThan, Int16x8>), 2)                     \
  V(greaterThanOrEqual, (CompareFunc<Int16x8, GreaterThanOrEqual, Int16x8>), 2)       \
  V(lessThan, (CompareFunc<Int16x8, LessThan, Int16x8>), 2)                           \
  V(lessThanOrEqual, (CompareFunc<Int16x8, LessThanOrEqual, Int16x8>), 2)             \
  V(load, (Load<Int16x8, 8>), 2)                                                      \
  V(mul, (BinaryFunc<Int16x8, Mul, Int16x8>), 2)                                      \
  V(notEqual, (CompareFunc<Int16x8, NotEqual, Int16x8>), 2)                           \
  V(or, (BinaryFunc<Int16x8, Or, Int16x8>), 2)                                        \
  V(sub, (BinaryFunc<Int16x8, Sub, Int16x8>), 2)                                      \
  V(shiftLeftByScalar, (BinaryScalar<Int16x8, ShiftLeft>), 2)                         \
  V(shiftRightArithmeticByScalar, (BinaryScalar<Int16x8, ShiftRightArithmetic>), 2)   \
  V(shiftRightLogicalByScalar, (BinaryScalar<Int16x8, ShiftRightLogical>), 2)         \
  V(xor, (BinaryFunc<Int16x8, Xor, Int16x8>), 2)

#define INT16X8_TERNARY_FUNCTION_LIST(V)                                              \
  V(replaceLane, (ReplaceLane<Int16x8>), 3)                                           \
  V(select, (Select<Int16x8, Int16x8>), 3)                                            \
  V(selectBits, (SelectBits<Int16x8, Int16x8>), 3)                                            \
  V(store, (Store<Int16x8, 8>), 3)

#define INT16X8_SHUFFLE_FUNCTION_LIST(V)                                              \
  V(swizzle, Swizzle<Int16x8>, 9)                                                     \
  V(shuffle, Shuffle<Int16x8>, 10)

#define INT16X8_FUNCTION_LIST(V)                                                      \
  INT16X8_UNARY_FUNCTION_LIST(V)                                                      \
  INT16X8_BINARY_FUNCTION_LIST(V)                                                     \
  INT16X8_TERNARY_FUNCTION_LIST(V)                                                    \
  INT16X8_SHUFFLE_FUNCTION_LIST(V)

#define INT32X4_UNARY_FUNCTION_LIST(V)                                                \
  V(check, (UnaryFunc<Int32x4, Identity, Int32x4>), 1)                                \
  V(fromFloat32x4, (FuncConvert<Float32x4, Int32x4>), 1)                              \
  V(fromFloat32x4Bits, (FuncConvertBits<Float32x4, Int32x4>), 1)                      \
  V(fromFloat64x2, (FuncConvert<Float64x2, Int32x4>), 1)                              \
  V(fromFloat64x2Bits, (FuncConvertBits<Float64x2, Int32x4>), 1)                      \
  V(fromInt8x16Bits, (FuncConvertBits<Int8x16, Int32x4>), 1)                          \
  V(fromInt16x8Bits, (FuncConvertBits<Int16x8, Int32x4>), 1)                          \
  V(neg, (UnaryFunc<Int32x4, Neg, Int32x4>), 1)                                       \
  V(not, (UnaryFunc<Int32x4, Not, Int32x4>), 1)                                       \
  V(splat, (FuncSplat<Int32x4>), 0)

#define INT32X4_BINARY_FUNCTION_LIST(V)                                               \
  V(add, (BinaryFunc<Int32x4, Add, Int32x4>), 2)                                      \
  V(and, (BinaryFunc<Int32x4, And, Int32x4>), 2)                                      \
  V(equal, (CompareFunc<Int32x4, Equal, Int32x4>), 2)                                 \
  V(extractLane, (ExtractLane<Int32x4>), 2)                                           \
  V(greaterThan, (CompareFunc<Int32x4, GreaterThan, Int32x4>), 2)                     \
  V(greaterThanOrEqual, (CompareFunc<Int32x4, GreaterThanOrEqual, Int32x4>), 2)       \
  V(lessThan, (CompareFunc<Int32x4, LessThan, Int32x4>), 2)                           \
  V(lessThanOrEqual, (CompareFunc<Int32x4, LessThanOrEqual, Int32x4>), 2)             \
  V(load,  (Load<Int32x4, 4>), 2)                                                     \
  V(load3, (Load<Int32x4, 3>), 2)                                                     \
  V(load2, (Load<Int32x4, 2>), 2)                                                     \
  V(load1, (Load<Int32x4, 1>), 2)                                                     \
  V(mul, (BinaryFunc<Int32x4, Mul, Int32x4>), 2)                                      \
  V(notEqual, (CompareFunc<Int32x4, NotEqual, Int32x4>), 2)                           \
  V(or, (BinaryFunc<Int32x4, Or, Int32x4>), 2)                                        \
  V(sub, (BinaryFunc<Int32x4, Sub, Int32x4>), 2)                                      \
  V(shiftLeftByScalar, (BinaryScalar<Int32x4, ShiftLeft>), 2)                         \
  V(shiftRightArithmeticByScalar, (BinaryScalar<Int32x4, ShiftRightArithmetic>), 2)   \
  V(shiftRightLogicalByScalar, (BinaryScalar<Int32x4, ShiftRightLogical>), 2)         \
  V(xor, (BinaryFunc<Int32x4, Xor, Int32x4>), 2)

#define INT32X4_TERNARY_FUNCTION_LIST(V)                                              \
  V(replaceLane, (ReplaceLane<Int32x4>), 3)                                           \
  V(select, (Select<Int32x4, Int32x4>), 3)                                            \
  V(selectBits, (SelectBits<Int32x4, Int32x4>), 3)                                            \
  V(store,  (Store<Int32x4, 4>), 3)                                                   \
  V(store3, (Store<Int32x4, 3>), 3)                                                   \
  V(store2, (Store<Int32x4, 2>), 3)                                                   \
  V(store1, (Store<Int32x4, 1>), 3)

#define INT32X4_SHUFFLE_FUNCTION_LIST(V)                                              \
  V(swizzle, Swizzle<Int32x4>, 5)                                                     \
  V(shuffle, Shuffle<Int32x4>, 6)

#define INT32X4_FUNCTION_LIST(V)                                                      \
  INT32X4_UNARY_FUNCTION_LIST(V)                                                      \
  INT32X4_BINARY_FUNCTION_LIST(V)                                                     \
  INT32X4_TERNARY_FUNCTION_LIST(V)                                                    \
  INT32X4_SHUFFLE_FUNCTION_LIST(V)

#define CONVERSION_INT32X4_SIMD_OP(_) \
    _(fromFloat32x4)                  \
    _(fromFloat32x4Bits)
#define FOREACH_INT32X4_SIMD_OP(_)   \
    CONVERSION_INT32X4_SIMD_OP(_)    \
    _(selectBits)                    \
    _(shiftLeftByScalar)             \
    _(shiftRightArithmeticByScalar)  \
    _(shiftRightLogicalByScalar)
#define UNARY_ARITH_FLOAT32X4_SIMD_OP(_) \
    _(abs)                           \
    _(sqrt)                          \
    _(reciprocalApproximation)       \
    _(reciprocalSqrtApproximation)
#define BINARY_ARITH_FLOAT32X4_SIMD_OP(_) \
    _(div)                           \
    _(max)                           \
    _(min)                           \
    _(maxNum)                        \
    _(minNum)
#define FOREACH_FLOAT32X4_SIMD_OP(_) \
    UNARY_ARITH_FLOAT32X4_SIMD_OP(_) \
    BINARY_ARITH_FLOAT32X4_SIMD_OP(_)\
    _(fromInt32x4)                   \
    _(fromInt32x4Bits)
#define ARITH_COMMONX4_SIMD_OP(_)    \
    _(add)                           \
    _(sub)                           \
    _(mul)
#define BITWISE_COMMONX4_SIMD_OP(_)  \
    _(and)                           \
    _(or)                            \
    _(xor)
#define COMP_COMMONX4_TO_INT32X4_SIMD_OP(_) \
    _(lessThan)                      \
    _(lessThanOrEqual)               \
    _(equal)                         \
    _(notEqual)                      \
    _(greaterThan)                   \
    _(greaterThanOrEqual)
// TODO: remove when all SIMD calls are inlined (bug 1112155)
#define ION_COMMONX4_SIMD_OP(_)      \
    ARITH_COMMONX4_SIMD_OP(_)        \
    BITWISE_COMMONX4_SIMD_OP(_)      \
    _(extractLane)                   \
    _(replaceLane)                   \
    _(select)                        \
    _(splat)                         \
    _(not)                           \
    _(neg)                           \
    _(swizzle)                       \
    _(shuffle)                       \
    _(load)                          \
    _(load1)                         \
    _(load2)                         \
    _(load3)                         \
    _(store)                         \
    _(store1)                        \
    _(store2)                        \
    _(store3)                        \
    _(check)
#define FOREACH_COMMONX4_SIMD_OP(_)  \
    ION_COMMONX4_SIMD_OP(_)          \
    COMP_COMMONX4_TO_INT32X4_SIMD_OP(_)
#define FORALL_SIMD_OP(_)            \
    FOREACH_INT32X4_SIMD_OP(_)       \
    FOREACH_FLOAT32X4_SIMD_OP(_)     \
    FOREACH_COMMONX4_SIMD_OP(_)

namespace js {

class SIMDObject : public JSObject
{
  public:
    static const Class class_;
    static bool toString(JSContext* cx, unsigned int argc, Value* vp);
};

// These classes implement the concept containing the following constraints:
// - requires typename Elem: this is the scalar lane type, stored in each lane
// of the SIMD vector.
// - requires static const unsigned lanes: this is the number of lanes (length)
// of the SIMD vector.
// - requires static const SimdTypeDescr::Type type: this is the SimdTypeDescr
// enum value corresponding to the SIMD type.
// - requires static bool Cast(JSContext*, JS::HandleValue, Elem*): casts a
// given Value to the current scalar lane type and saves it in the Elem
// out-param.
// - requires static Value ToValue(Elem): returns a Value of the right type
// containing the given value.
//
// This concept is used in the templates above to define the functions
// associated to a given type and in their implementations, to avoid code
// redundancy.

struct Float32x4 {
    typedef float Elem;
    static const unsigned lanes = 4;
    static const SimdTypeDescr::Type type = SimdTypeDescr::Float32x4;
    static bool Cast(JSContext* cx, JS::HandleValue v, Elem* out) {
        double d;
        if (!ToNumber(cx, v, &d))
            return false;
        *out = float(d);
        return true;
    }
    static Value ToValue(Elem value) {
        return DoubleValue(JS::CanonicalizeNaN(value));
    }
};

struct Float64x2 {
    typedef double Elem;
    static const unsigned lanes = 2;
    static const SimdTypeDescr::Type type = SimdTypeDescr::Float64x2;
    static bool Cast(JSContext* cx, JS::HandleValue v, Elem* out) {
        return ToNumber(cx, v, out);
    }
    static Value ToValue(Elem value) {
        return DoubleValue(JS::CanonicalizeNaN(value));
    }
};

struct Int8x16 {
    typedef int8_t Elem;
    static const unsigned lanes = 16;
    static const SimdTypeDescr::Type type = SimdTypeDescr::Int8x16;
    static bool Cast(JSContext* cx, JS::HandleValue v, Elem* out) {
        return ToInt8(cx, v, out);
    }
    static Value ToValue(Elem value) {
        return Int32Value(value);
    }
};

struct Int16x8 {
    typedef int16_t Elem;
    static const unsigned lanes = 8;
    static const SimdTypeDescr::Type type = SimdTypeDescr::Int16x8;
    static bool Cast(JSContext* cx, JS::HandleValue v, Elem* out) {
        return ToInt16(cx, v, out);
    }
    static Value ToValue(Elem value) {
        return Int32Value(value);
    }
};

struct Int32x4 {
    typedef int32_t Elem;
    static const unsigned lanes = 4;
    static const SimdTypeDescr::Type type = SimdTypeDescr::Int32x4;
    static bool Cast(JSContext* cx, JS::HandleValue v, Elem* out) {
        return ToInt32(cx, v, out);
    }
    static Value ToValue(Elem value) {
        return Int32Value(value);
    }
};

template<typename V>
JSObject* CreateSimd(JSContext* cx, const typename V::Elem* data);

template<typename V>
bool IsVectorObject(HandleValue v);

template<typename V>
bool ToSimdConstant(JSContext* cx, HandleValue v, jit::SimdConstant* out);

#define DECLARE_SIMD_FLOAT32X4_FUNCTION(Name, Func, Operands)   \
extern bool                                                     \
simd_float32x4_##Name(JSContext* cx, unsigned argc, Value* vp);
FLOAT32X4_FUNCTION_LIST(DECLARE_SIMD_FLOAT32X4_FUNCTION)
#undef DECLARE_SIMD_FLOAT32X4_FUNCTION

#define DECLARE_SIMD_FLOAT64X2_FUNCTION(Name, Func, Operands)   \
extern bool                                                     \
simd_float64x2_##Name(JSContext* cx, unsigned argc, Value* vp);
FLOAT64X2_FUNCTION_LIST(DECLARE_SIMD_FLOAT64X2_FUNCTION)
#undef DECLARE_SIMD_FLOAT64X2_FUNCTION

#define DECLARE_SIMD_INT8X16_FUNCTION(Name, Func, Operands)     \
extern bool                                                     \
simd_int8x16_##Name(JSContext* cx, unsigned argc, Value* vp);
INT8X16_FUNCTION_LIST(DECLARE_SIMD_INT8X16_FUNCTION)
#undef DECLARE_SIMD_INT8X16_FUNCTION

#define DECLARE_SIMD_INT16X8_FUNCTION(Name, Func, Operands)     \
extern bool                                                     \
simd_int16x8_##Name(JSContext* cx, unsigned argc, Value* vp);
INT16X8_FUNCTION_LIST(DECLARE_SIMD_INT16X8_FUNCTION)
#undef DECLARE_SIMD_INT16X8_FUNCTION

#define DECLARE_SIMD_INT32x4_FUNCTION(Name, Func, Operands)     \
extern bool                                                     \
simd_int32x4_##Name(JSContext* cx, unsigned argc, Value* vp);
INT32X4_FUNCTION_LIST(DECLARE_SIMD_INT32x4_FUNCTION)
#undef DECLARE_SIMD_INT32x4_FUNCTION

JSObject*
InitSIMDClass(JSContext* cx, HandleObject obj);

}  /* namespace js */

#endif /* builtin_SIMD_h */
