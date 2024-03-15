/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "intgemm/IntegerGemmIntrinsic.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/IntegerPrintfMacros.h"

#include <gemmology_fwd.h>

#include "js/ErrorReport.h"
#include "js/HeapAPI.h"
#include "vm/ArrayBufferObject.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmLog.h"

#if defined(USE_AVX2)
#  define SUPPORTED_ARCHS \
    xsimd::arch_list<xsimd::avx2, xsimd::ssse3, xsimd::sse2>
#elif defined(USE_SSSE3)
#  define SUPPORTED_ARCHS xsimd::arch_list<xsimd::ssse3, xsimd::sse2>
#elif defined(USE_SSE2)
#  define SUPPORTED_ARCHS xsimd::arch_list<xsimd::sse2>
#elif defined(USE_NEON) and defined(XSIMD_WITH_NEON64)
#  define SUPPORTED_ARCHS xsimd::arch_list<xsimd::neon64>
#else
#  error no supported architecture
#endif

// Dispatch *at runtime* based on run-time hardware and compile-time
// architectures.
//
// FIXME: Ideally we would not run the dispatch code at each function call.
#define GEMMOLOGY_DISPATCH(FUNC)                                 \
  xsimd::dispatch<SUPPORTED_ARCHS>([](auto arch, auto... args) { \
    return gemmology::Engine<decltype(arch)>::FUNC(args...);     \
  })

struct JSContext;

static constexpr uint32_t ARRAY_ALIGNMENT = 64;
static constexpr uint32_t ROWS_A_MULTIPLIER = 1;
static constexpr uint32_t COLUMNS_A_MULTIPLIER = 64;
static constexpr uint32_t ROWS_B_MULTIPLIER = COLUMNS_A_MULTIPLIER;
static constexpr uint32_t COLUMNS_B_MULTIPLIER = 8;
static constexpr uint32_t SELECTED_COLUMNS_B_MULTIPLIER = 8;

void ReportGemmError(JSContext* cx, const unsigned errorNumber) {
  JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr, errorNumber);
}

size_t GetWasmRawBufferLength(const uint8_t* memBase) {
  const js::WasmArrayRawBuffer* rawBuf =
      js::WasmArrayRawBuffer::fromDataPtr(memBase);
  return rawBuf->byteLength();
}

bool CheckMatrixDimension(JSContext* cx, uint32_t size,
                          uint32_t sizeMultiplier) {
  // A valid size is a positive integral multiple of Multiplier
  if ((size == 0) || (size % sizeMultiplier != 0)) {
    js::wasm::Log(
        cx, "Invalid dimension value:%" PRIu32 " (should be a multiple of %u)",
        size, sizeMultiplier);
    return false;
  }
  return true;
}

bool CheckMatrixBound(JSContext* cx, uint32_t input, uint64_t inputSize,
                      size_t wasmBufferSize) {
  mozilla::CheckedUint64 inputUpperLimit(inputSize);
  inputUpperLimit += input;

  // Bound check fails if size overflows or it spans outside the wasm memory
  if (!inputUpperLimit.isValid() ||
      (inputUpperLimit.value() >= (uint64_t)wasmBufferSize)) {
    js::wasm::Log(cx, "Memory out of wasm bounds for matrix:%" PRIu32, input);
    return false;
  }
  return true;
}

bool CheckMatrixBoundAndAlignment(JSContext* cx, uint32_t input,
                                  uint64_t inputSize, size_t wasmBufferSize) {
  // Alignment check: It is sufficient to check alignment for the offset rather
  // than for the actual pointer within wasm memory (as long as following assert
  // is satisfied)
  static_assert(js::gc::PageSize >= ARRAY_ALIGNMENT,
                "PageSize should be bigger than Alignment");
  if (input % ARRAY_ALIGNMENT != 0) {
    js::wasm::Log(
        cx, "Unaligned access for matrix:%" PRIu32 " (should be %u aligned)",
        input, ARRAY_ALIGNMENT);
    return false;
  }

  // Check Bound
  return CheckMatrixBound(cx, input, inputSize, wasmBufferSize);
}

int32_t js::intgemm::IntrI8PrepareB(wasm::Instance* instance,
                                    uint32_t inputMatrixB, float scale,
                                    float zeroPoint, uint32_t rowsB,
                                    uint32_t colsB, uint32_t outputMatrixB,
                                    uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8PrepareB.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsB, ROWS_B_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsB, COLUMNS_B_MULTIPLIER)) {
    wasm::Log(cx, "%s: rowsB:%" PRIu32 "  colsB:%" PRIu32, __FUNCTION__, rowsB,
              colsB);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound and Alignment checks for matricies
  uint64_t sizeB = (uint64_t)rowsB * (uint64_t)colsB;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixB, sizeB, wasmBufferSize) ||
      !CheckMatrixBoundAndAlignment(cx, outputMatrixB, sizeB, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: inputB:%x  rowsB:%" PRIu32 "  colsB:%" PRIu32
              "  outputB:%x  sizeB:%" PRIu64 "  wasmBufferSize:%zu",
              __FUNCTION__, inputMatrixB, rowsB, colsB, outputMatrixB, sizeB,
              wasmBufferSize);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm) for PrepareB
  uint8_t* inputMatrixBPtr = &memBase[inputMatrixB];
  uint8_t* outputMatrixBPtr = &memBase[outputMatrixB];
  GEMMOLOGY_DISPATCH(PrepareB)
  ((const float*)inputMatrixBPtr, (int8_t*)outputMatrixBPtr,
   (float)scale,  // Quant Mult
   rowsB, colsB);
  return 0;
}

int32_t js::intgemm::IntrI8PrepareBFromTransposed(
    wasm::Instance* instance, uint32_t inputMatrixBTransposed, float scale,
    float zeroPoint, uint32_t rowsB, uint32_t colsB, uint32_t outputMatrixB,
    uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8PrepareBFromTransposed.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsB, ROWS_B_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsB, COLUMNS_B_MULTIPLIER)) {
    wasm::Log(cx, "%s: rowsB:%" PRIu32 "  colsB:%" PRIu32, __FUNCTION__, rowsB,
              colsB);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound checks for all matricies
  uint64_t sizeB = (uint64_t)rowsB * (uint64_t)colsB;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixBTransposed, sizeB,
                                    wasmBufferSize) ||
      !CheckMatrixBoundAndAlignment(cx, outputMatrixB, sizeB, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: inputBT:%x  rowsB:%" PRIu32 "  colsB:%" PRIu32
              "  outputB:%x  sizeB:%" PRIu64 "  wasmBufferSize:%zu",
              __FUNCTION__, inputMatrixBTransposed, rowsB, colsB, outputMatrixB,
              sizeB, wasmBufferSize);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm) for PrepareBTransposed
  uint8_t* inputMatrixBTransposedPtr = &memBase[inputMatrixBTransposed];
  uint8_t* outputMatrixBPtr = &memBase[outputMatrixB];
  GEMMOLOGY_DISPATCH(PrepareBTransposed)
  ((const float*)inputMatrixBTransposedPtr, (int8_t*)outputMatrixBPtr,
   (float)scale,  // Quant Mult
   rowsB, colsB);
  return 0;
}

int32_t js::intgemm::IntrI8PrepareBFromQuantizedTransposed(
    wasm::Instance* instance, uint32_t inputMatrixBQuantizedTransposed,
    uint32_t rowsB, uint32_t colsB, uint32_t outputMatrixB, uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8PrepareBFromQuantizedTransposed.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsB, ROWS_B_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsB, COLUMNS_B_MULTIPLIER)) {
    wasm::Log(cx, "%s: rowsB:%" PRIu32 "  colsB:%" PRIu32, __FUNCTION__, rowsB,
              colsB);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound checks for all matricies
  uint64_t sizeB = (uint64_t)rowsB * (uint64_t)colsB;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixBQuantizedTransposed, sizeB,
                                    wasmBufferSize) ||
      !CheckMatrixBoundAndAlignment(cx, outputMatrixB, sizeB, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: inputBQT:%x  rowsB:%" PRIu32 "  colsB:%" PRIu32
              "  outputB:%x  sizeA:%" PRIu64 "  wasmBufferSize:%zu",
              __FUNCTION__, inputMatrixBQuantizedTransposed, rowsB, colsB,
              outputMatrixB, sizeB, wasmBufferSize);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm)
  uint8_t* inputMatrixBQuantizedTransposedPtr =
      &memBase[inputMatrixBQuantizedTransposed];
  uint8_t* outputMatrixBPtr = &memBase[outputMatrixB];
  GEMMOLOGY_DISPATCH(PrepareBQuantizedTransposed)
  ((const int8_t*)inputMatrixBQuantizedTransposedPtr, (int8_t*)outputMatrixBPtr,
   rowsB, colsB);
  return 0;
}

int32_t js::intgemm::IntrI8PrepareA(wasm::Instance* instance,
                                    uint32_t inputMatrixA, float scale,
                                    float zeroPoint, uint32_t rowsA,
                                    uint32_t colsA, uint32_t outputMatrixA,
                                    uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8PrepareA.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsA, ROWS_A_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsA, COLUMNS_A_MULTIPLIER)) {
    wasm::Log(cx, "%s: rowsA:%" PRIu32 "  colsA:%" PRIu32, __FUNCTION__, rowsA,
              colsA);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound checks for all matricies
  uint64_t sizeA = (uint64_t)rowsA * (uint64_t)colsA;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixA, sizeA, wasmBufferSize) ||
      !CheckMatrixBoundAndAlignment(cx, outputMatrixA, sizeA, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: inputA:%x  rowsA:%" PRIu32 "  colsA:%" PRIu32
              "  outputA:%x  sizeA:%" PRIu64 "  wasmBufferSize:%zu",
              __FUNCTION__, inputMatrixA, rowsA, colsA, outputMatrixA, sizeA,
              wasmBufferSize);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm)
  uint8_t* inputMatrixAPtr = &memBase[inputMatrixA];
  uint8_t* outputMatrixAPtr = &memBase[outputMatrixA];
  GEMMOLOGY_DISPATCH(Shift::PrepareA)
  ((const float*)inputMatrixAPtr, outputMatrixAPtr, scale, rowsA, colsA);
  return 0;
}

int32_t js::intgemm::IntrI8PrepareBias(
    wasm::Instance* instance, uint32_t inputMatrixBPrepared, float scaleA,
    float zeroPointA, float scaleB, float zeroPointB, uint32_t rowsB,
    uint32_t colsB, uint32_t inputBias, uint32_t output, uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8PrepareBias.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsB, ROWS_B_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsB, COLUMNS_B_MULTIPLIER)) {
    wasm::Log(cx, "%s: rowsB:%" PRIu32 "  colsB:%" PRIu32, __FUNCTION__, rowsB,
              colsB);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound checks for all matricies
  uint64_t sizeB = (uint64_t)rowsB * (uint64_t)colsB;
  uint64_t sizeBias = colsB;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixBPrepared, sizeB,
                                    wasmBufferSize) ||
      !CheckMatrixBound(cx, inputBias, sizeBias, wasmBufferSize) ||
      !CheckMatrixBound(cx, output, sizeBias, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: preparedB:%x  rowsB:%" PRIu32 "  colsB:%" PRIu32
              "  inputBias:%x  outputBias:%x  sizeB:%" PRIu64
              "  wasmBufferSize:%zu",
              __FUNCTION__, inputMatrixBPrepared, rowsB, colsB, inputBias,
              output, sizeB, wasmBufferSize);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm)
  uint8_t* inputMatrixBPreparedPtr = &memBase[inputMatrixBPrepared];
  uint8_t* inputBiasPtr = &memBase[inputBias];
  uint8_t* outputPtr = &memBase[output];
  float unquantFactor =
      (-1) * ((127.0f / scaleA) * (127.0f / scaleB)) / (127.0f);
  GEMMOLOGY_DISPATCH(Shift::PrepareBias)
  ((const int8_t*)inputMatrixBPreparedPtr, rowsB, colsB,
   gemmology::callbacks::UnquantizeAndAddBiasAndWrite(
       unquantFactor, (const float*)inputBiasPtr, (float*)outputPtr));
  return 0;
}

int32_t js::intgemm::IntrI8MultiplyAndAddBias(
    wasm::Instance* instance, uint32_t inputMatrixAPrepared, float scaleA,
    float zeroPointA, uint32_t inputMatrixBPrepared, float scaleB,
    float zeroPointB, uint32_t inputBiasPrepared, float unquantMultiplier,
    uint32_t rowsA, uint32_t width, uint32_t colsB, uint32_t output,
    uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8MultiplyAndAddBias.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsA, ROWS_A_MULTIPLIER) ||
      !CheckMatrixDimension(cx, width, COLUMNS_A_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsB, COLUMNS_B_MULTIPLIER)) {
    wasm::Log(cx, "%s: rowsA:%" PRIu32 "  width:%" PRIu32 "  colsB:%" PRIu32,
              __FUNCTION__, rowsA, width, colsB);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound checks for all matricies
  uint64_t sizeA = (uint64_t)rowsA * (uint64_t)width;
  uint64_t sizeB = (uint64_t)width * (uint64_t)colsB;
  uint64_t sizeBias = (uint64_t)colsB;
  uint64_t sizeOutput = (uint64_t)rowsA * (uint64_t)colsB;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixAPrepared, sizeA,
                                    wasmBufferSize) ||
      !CheckMatrixBoundAndAlignment(cx, inputMatrixBPrepared, sizeB,
                                    wasmBufferSize) ||
      !CheckMatrixBound(cx, inputBiasPrepared, sizeBias, wasmBufferSize) ||
      !CheckMatrixBound(cx, output, sizeOutput, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: preparedA:%x  preparedB:%x  preparedBias:%x  rowsA:%" PRIu32
              "  width:%" PRIu32 "  colsB:%" PRIu32
              "  output:%x  sizeA:%" PRIu64 "  sizeB:%" PRIu64
              "  sizeBias:%" PRIu64 "  sizeOutput:%" PRIu64,
              __FUNCTION__, inputMatrixAPrepared, inputMatrixBPrepared,
              inputBiasPrepared, rowsA, width, colsB, output, sizeA, sizeB,
              sizeBias, sizeOutput);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm)
  uint8_t* inputMatrixAPreparedPtr = &memBase[inputMatrixAPrepared];
  uint8_t* inputMatrixBPreparedPtr = &memBase[inputMatrixBPrepared];
  uint8_t* inputBiasPreparedPtr = &memBase[inputBiasPrepared];
  uint8_t* outputPtr = &memBase[output];
  float unquantFactor = unquantMultiplier / (scaleA * scaleB);

  GEMMOLOGY_DISPATCH(Shift::Multiply)
  (inputMatrixAPreparedPtr, (const int8_t*)inputMatrixBPreparedPtr, rowsA,
   width, colsB,
   gemmology::callbacks::UnquantizeAndAddBiasAndWrite(
       unquantFactor, (const float*)inputBiasPreparedPtr, (float*)outputPtr));
  return 0;
}

int32_t js::intgemm::IntrI8SelectColumnsOfB(wasm::Instance* instance,
                                            uint32_t inputMatrixBPrepared,
                                            uint32_t rowsB, uint32_t colsB,
                                            uint32_t colIndexList,
                                            uint32_t sizeColIndexList,
                                            uint32_t output, uint8_t* memBase) {
  MOZ_ASSERT(wasm::SASigIntrI8SelectColumnsOfB.failureMode ==
             wasm::FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Size checks for matricies
  if (!CheckMatrixDimension(cx, rowsB, ROWS_B_MULTIPLIER) ||
      !CheckMatrixDimension(cx, colsB, COLUMNS_B_MULTIPLIER) ||
      !CheckMatrixDimension(cx, sizeColIndexList,
                            SELECTED_COLUMNS_B_MULTIPLIER)) {
    wasm::Log(cx,
              "%s: rowsB:%" PRIu32 "  colsB:%" PRIu32
              "  sizeColIndexList:%" PRIu32,
              __FUNCTION__, rowsB, colsB, sizeColIndexList);
    ReportGemmError(cx, JSMSG_WASM_UNREACHABLE);
    return -1;
  }

  // Memory Bound checks for all matricies
  uint64_t sizeB = (uint64_t)rowsB * (uint64_t)colsB;
  uint64_t sizeOutput = (uint64_t)rowsB * (uint64_t)sizeColIndexList;
  size_t wasmBufferSize = GetWasmRawBufferLength(memBase);
  if (!CheckMatrixBoundAndAlignment(cx, inputMatrixBPrepared, sizeB,
                                    wasmBufferSize) ||
      !CheckMatrixBound(cx, colIndexList, sizeColIndexList, wasmBufferSize) ||
      !CheckMatrixBound(cx, output, sizeOutput, wasmBufferSize)) {
    wasm::Log(cx,
              "%s: preparedB:%x  rowsB:%" PRIu32 "  colsB:%" PRIu32
              "  colList:%x  sizeColList:%" PRIu32 " output:%x  sizeB:%" PRIu64
              "  sizeOutput:%" PRIu64,
              __FUNCTION__, inputMatrixBPrepared, rowsB, colsB, colIndexList,
              sizeColIndexList, output, sizeB, sizeOutput);
    ReportGemmError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Actual call to the 3rd party library (intgemm)
  uint8_t* inputMatrixBPreparedPtr = &memBase[inputMatrixBPrepared];
  uint8_t* colIndexListPtr = &memBase[colIndexList];
  uint8_t* outputPtr = &memBase[output];
  GEMMOLOGY_DISPATCH(SelectColumnsB)
  ((const int8_t*)inputMatrixBPreparedPtr, (int8_t*)outputPtr, rowsB,
   (const uint32_t*)colIndexListPtr,
   (const uint32_t*)colIndexListPtr + sizeColIndexList);
  return 0;
}

#undef GEMMOLOGY_DISPATCH
#undef SUPPORTED_ARCHS
