/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef intgemm_IntegerGemmIntrinsic_h
#define intgemm_IntegerGemmIntrinsic_h

#include <stdint.h>

namespace js {
namespace wasm {
class Instance;
}

namespace intgemm {

/* Interface for integer matrix multiplication followed by addition of bias.
 *
 * C = A * B + Bias
 *
 * Input matrix A:
 *  - A 2-D matrix that typically represents activations as floating point
 * values
 *  - no. of rows should be a positive integer
 *  - no. of columns should be a positive integeral multiple of 64
 *  - is represented as array (contiguous memory locations) in row-major format
 *
 * Input matrix B:
 *  - A 2-D matrix that typically represents fixed model parameters as
 * floating point values
 *  - no. of rows should be:
 *    -- equal to no. of columns of Input matrix A
 *    -- a positive integeral multiple of 64
 *  - no. of columns should be a positive integeral multiple of 8
 *  - is represented as array (contiguous memory locations) in row-major format
 *
 *  Please note that it is also possible to pass Input matrix B in 2 more forms:
 *   - One that is already a quantized and transposed version of Input matrix B
 *   - Other that is already a transposed version of Input matrix B
 *
 * Input Bias:
 *  - is an array (contiguous memory locations) that represents bias
 *  - size of the array should be equal to the no. of columns of Input matrix B
 *
 * Output matrix C:
 *  - is a 2-D matrix that represents the result (= A * B + Bias)
 *  - no. of rows = no. of rows of Input matrix A
 *  - no. of columns = no. of columns of Input matrix B (in
 * untransposed form)
 *  - is represented as array (contiguous memory locations) in row-major format
 *
 * Please note that most of the functions in this interface might have
 * architecture specific implementations.
 *
 * Conventions followed for the interface:
 *  - Unless explicitly mentioned, Input matrix B refers to an unquantized
 * (i.e. float values) and non-transposed version
 *  - no. of rows of Input matrix A = `rowsA`
 *  - no. of columns of Input matrix A (`colsA`) = no. of rows of Input matrix B
 * (`rowsB`) = `width`
 *  - no. of columns of Input matrix B = `colsB`
 */

/* Prepare B for the Matrix Multiply function from Input matrix B.
 *
 * Quantization is performed on the input.
 * The final prepared B is in CPU-dependent format and can be used as an input
 * to matrix multiply function (`int8_multiply_and_add_bias`).
 *
 * Please note that this interface might have architecture specific
 * implementation.
 *
 * @param[in]   inputMatrixB        An array representing the Input matrix B in
 *                                  row-major format.
 *                                  Size of the array = `rowsB` * `colsB`.
 *                                  Shape of the matrix: (`rowsB`, `colsB`)
 * @param[in]   scale               The scaling factor (for quantization)
 * @param[in]   zeroPoint           The zero point (for quantization)
 * @param[in]   rowsB               No. of rows of Input matrix B. It should be
 *                                  a positive integer and a multiple of 64.
 * @param[in]   colsB               No. of columns of Input matrix B. It should
 *                                  be a positive integer and a multiple of 8.
 * @param[out]  outputMatrixB       An array representing the prepared B matrix.
 *                                  Size of the array = `rowsB` * `colsB`.
 *
 * This function implements the intrinsic:
 *   int8_prepare_b(inputMatrixB: i32, scale: f32, zeroPoint: f32, rowsB: i32,
 * colsB: i32, outputMatrixB: i32) which implements the function:
 *   int8_prepare_b(const float* inputMatrixB, float scale, float zeroPoint,
 * uint32_t rowsB, uint32_t colsB, int8_t* outputMatrixB)
 */
int32_t IntrI8PrepareB(wasm::Instance* instance, uint32_t inputMatrixB,
                       float scale, float zeroPoint, uint32_t rowsB,
                       uint32_t colsB, uint32_t outputMatrixB,
                       uint8_t* memBase);

/* Prepare B for the Matrix Multiply function from transposed version of Input
 * matrix B.
 *
 * Quantization is performed on floating values of input.
 * The final prepared B is in CPU-dependent format and can be used as an input
 * to matrix multiply function (`int8_multiply_and_add_bias`).
 *
 * Please note that this interface might have architecture specific
 * implementation.
 *
 * @param[in]   inputMatrixBTransposed An array representing transposed version
 *                                     of Input matrix B.
 *                                     It is in column-major format.
 *                                     Size of the array = `rowsB` * `colsB`.
 *                                     Shape of the matrix: (`colsB`, `rowsB`)
 * @param[in]   scale                  The scaling factor (for quantization)
 * @param[in]   zeroPoint              The zero point (for quantization)
 * @param[in]   rowsB                  No. of rows of Input matrix B. It should
 *                                     be a positive integer and a multiple of
 *                                     64.
 * @param[in]   colsB                  No. of columns of Input matrix B. It
 *                                     should be a positive integer and a
 *                                     multiple of 8.
 * @param[out]  outputMatrixB          An array representing the prepared B
 *                                     matrix. Size of array = `rowsB`*`colsB`
 *
 * This function implements the intrinsic:
 *   int8_prepare_b_from_transposed(inputMatrixBTransposed: i32, scale: f32,
 * zeroPoint: f32, rowsB: i32, colsB: i32, outputMatrixB: i32) which implements
 * the function: int8_prepare_b_from_transposed(const float*
 * inputMatrixBTransposed, float scale, float zeroPoint, uint32_t rowsB,
 * uint32_t colsB, int8_t* outputMatrixB)
 */
int32_t IntrI8PrepareBFromTransposed(wasm::Instance* instance,
                                     uint32_t inputMatrixBTransposed,
                                     float scale, float zeroPoint,
                                     uint32_t rowsB, uint32_t colsB,
                                     uint32_t outputMatrixB, uint8_t* memBase);

/* Prepare B for the Matrix Multiply function from a quantized and transposed
 * version of Input matrix B which is also in a CPU-independent format.
 *
 * The final prepared B is in CPU-dependent format and can be used as an input
 * to matrix multiply function (`int8_multiply_and_add_bias`).
 *
 * This function is useful while using the quantized models that are stored in a
 * CPU-independent format on the disk.
 *
 * @param[in]   inputMatrixBQuantizedTransposed  An array representing the
 *                                               quantized and transposed
 *                                               version of Input matrix B.
 *                                               It is in column-major format.
 *                                               Size of array =
 *                                                 `rowsB`*`colsB`
 *                                               Shape of the matrix:
 *                                                 (`colsB`,`rowsB`)
 * @param[in]   rowsB                            No. of rows of Input matrix B.
 *                                               Should be a positive integer
 *                                               and a multiple of 64.
 * @param[in]   colsB                            No. of columns of Input matrix
 *                                               B. Should be a positive
 *                                               integer and a multiple of 8
 * @param[out]  outputMatrixB                    An array representing the
 *                                               prepared B matrix.
 *                                               Size: `rowsB` * `colsB`.
 *
 * This function implements the intrinsic:
 *   int8_prepare_b_from_quantized_transposed(inputMatrixBQuantizedTransposed:
 * i32, rowsB: i32, colsB: i32, outputMatrixB: i32) which implements the
 * function: int8_prepare_b_from_quantized_transposed(const int8_t*
 * inputMatrixBQuantizedTransposed, uint32_t rowsB, uint32_t colsB, int8_t*
 * outputMatrixB)
 */
int32_t IntrI8PrepareBFromQuantizedTransposed(
    wasm::Instance* instance, uint32_t inputMatrixBQuantizedTransposed,
    uint32_t rowsB, uint32_t colsB, uint32_t outputMatrixB, uint8_t* memBase);

/* Prepare A for the Matrix Multiply function from Input matrix A.
 *
 * It performs quantization on floating values of input.
 * The final prepared A might be architecture dependent. e.g. On some
 * architectures like x86, it might be unsigned (achieved by adding 127 to
 * quantized values) while on others like Arm, it might be signed. The final
 * prepared A can be used as an input to matrix multiply function
 * (`int8_multiply_and_add_bias`).
 *
 * Please note that this interface might have architecture specific
 * implementation.
 *
 * @param[in]   inputMatrixA   An array representing the Input matrix A in
 *                             row-major format.
 *                             Size of the array = `rowsA` * `colsA`.
 *                             Shape of the matrix: (`rowsA`, `colsA`)
 * @param[in]   scale          The scaling factor (for quantization)
 * @param[in]   zeroPoint      The zero point (for quantization)
 * @param[in]   rowsA          No. of rows of Input matrix A. It should be a
 *                             positive integer.
 * @param[in]   colsA          No. of columns of Input matrix A. It should be a
 *                             positive integer and a multiple of 64.
 * @param[out]  outputMatrixA  An array representing the prepared A matrix.
 *                             Size of the array = `rowsA` * `colsA`.
 *
 * This function implements the intrinsic:
 *   int8_prepare_a(inputMatrixA: i32, scale: f32, zeroPoint: f32, rowsA: i32,
 * colsA: i32, outputMatrixA: i32) which implements the function:
 *   int8_prepare_a(const float* inputMatrixA, float scale, float zeroPoint,
 * uint32_t rowsA, uint32_t colsA, int8_t* outputMatrixA)
 */
int32_t IntrI8PrepareA(wasm::Instance* instance, uint32_t inputMatrixA,
                       float scale, float zeroPoint, uint32_t rowsA,
                       uint32_t colsA, uint32_t outputMatrixA,
                       uint8_t* memBase);

/* Prepares bias for the Matrix Multiply function.
 *
 * It uses the prepared B (which must be obtained by using any of the
 * int8_prepare_b* functions) and a bias input to prepare the final bias.
 *
 * The final bias can be used as an input to matrix multiply function
 * (`int8_multiply_and_add_bias`).
 *
 * @param[in]   inputMatrixBPrepared An array representing the prepared B
 *                                   matrix. Size of array = `rowsB`*`colsB`.
 * @param[in]   scaleA               The scaling factor (for quantization) of A
 * @param[in]   zeroPointA           The zero point (for quantization) of A
 * @param[in]   scaleB               The scaling factor (for quantization) of B
 * @param[in]   zeroPointB           The zero point (for quantization) of B
 * @param[in]   rowsB                No. of rows of Input matrix B (unquantized
 *                                   & non-transposed). It should be a positive
 *                                   integer and a multiple of 64.
 * @param[in]   colsB                No. of columns of Input matrix B
 *                                   (unquantized & non-transposed). It should
 *                                   be a positive integer and a multiple of 8.
 * @param[in]   inputBias            An array representing the input bias. Size
 *                                   of array = `colsB`
 * @param[out]  output               An array representing the final prepared
 *                                   bias. Size of the array = `colsB`
 *
 * This function implements the intrinsic:
 *   int8_prepare_bias(inputMatrixBPrepared: i32, scaleA: f32, zeroPointA: f32,
 * scaleB: f32, zeroPointB: f32, rowsB: i32, colsB: i32, inputBias: i32, output:
 * i32) which implements the function: int8_prepare_bias(const int8_t*
 * inputMatrixBPrepared, float scaleA, float zeroPointA, float scaleB, float
 * zeroPointB, uint32_t rowsB, uint32_t colsB, const float* inputBias, float*
 * output)
 */
int32_t IntrI8PrepareBias(wasm::Instance* instance,
                          uint32_t inputMatrixBPrepared, float scaleA,
                          float zeroPointA, float scaleB, float zeroPointB,
                          uint32_t rowsB, uint32_t colsB, uint32_t inputBias,
                          uint32_t output, uint8_t* memBase);

/* Perform multiplication of 2 matrices followed by adding a bias.
 *
 * i.e Output = inputMatrixAPrepared * inputMatrixBPrepared + inputBiasPrepared
 *
 * The inputs inputMatrixAPrepared, inputMatrixBPrepared and inputBiasPrepared
 * of this function must be obtained by using `int8_prepare_A`, one of the
 * `int8_prepare_b*` and `int8_prepare_bias` functions respectively.
 *
 * Please note that this interface might have architecture specific
 * implementation.
 *
 * @param[in]   inputMatrixAPrepared   An array representing the prepared A
 *                                     matrix. This must be obtained by using
 *                                     `int8_prepare_A` function. Size of the
 *                                     array = `rowsA` * `width`.
 * @param[in]   scaleA                 The scaling factor (quantization) of A
 * @param[in]   zeroPointA             The zero point (for quantization) of A
 * @param[in]   inputMatrixBPrepared   An array representing the prepared B
 *                                     matrix. This must be obtained by using
 *                                     one of `int8_prepare_b*` functions.
 *                                     Size of the array = `width` * `colsB`.
 * @param[in]   scaleB                 The scaling factor (quantization) of B
 * @param[in]   zeroPointB             The zero point (for quantization) of B
 * @param[in]   inputBiasPrepared      An array representing the prepared bias.
 *                                     This must be obtained by using
 *                                     `int8_prepare_bias` function.
 *                                     Size of the array = `colsB`
 * @param[in]   unquantMultiplier      A value that will be multiplied to the
 *                                     final unquantization factor that is
 *                                     prepared from `scaleA` and `scaleB`.
 * @param[in]   rowsA                  No. of rows of Input matrix A. It should
 *                                     be a positive integer.
 * @param[in]   width                  No. of columns of Input matrix A (same as
 *                                     no. of columns of Input matrix B). It
 *                                     should be a positive integer and a
 *                                     multiple of 64.
 * @param[in]   colsB                  No. of columns of Input matrix B. Should
 *                                     be a multiple of 8.
 * @param[out]  output                 An array representing the result matrix
 *                                     in row-major format.
 *                                     Size of the array = `rowsA` * `colsB`.
 *
 * This function implements the intrinsic:
 *   int8_multiply_and_add_bias(inputMatrixAPrepared: i32, scaleA: f32,
 * zeroPointA: f32, inputMatrixBPrepared: i32, scaleB: f32, zeroPointB: f32,
 *                     inputBiasPrepared: i32, unquantMultiplier: f32,
 *                     rowsA: i32, width: i32, colsB: i32, output: i32)
 * which implements the function:
 *   int8_multiply_and_add_bias(const uint8_t* inputMatrixAPrepared, float
 * scaleA, float zeroPointA, const int8_t* inputMatrixBPrepared, float scaleB,
 * float zeroPointB, const float* inputBiasPrepared, float unquantMultiplier,
 *                     uint32_t rowsA, uint32_t width, uint32_t colsB, float*
 * output)
 */
int32_t IntrI8MultiplyAndAddBias(wasm::Instance* instance,
                                 uint32_t inputMatrixAPrepared, float scaleA,
                                 float zeroPointA,
                                 uint32_t inputMatrixBPrepared, float scaleB,
                                 float zeroPointB, uint32_t inputBiasPrepared,
                                 float unquantMultiplier, uint32_t rowsA,
                                 uint32_t width, uint32_t colsB,
                                 uint32_t output, uint8_t* memBase);

/* Select a subset of columns of prepared B.
 *
 * Indices of the columns to be selected are specified by an array.
 *
 * @param[in]   inputMatrixBPrepared  An array representing the prepared B
 *                                    matrix. This must be obtained by using
 *                                    one of the `int8_prepare_b*` functions.
 *                                    Size of the array = `rowsB` * `colsB`.
 * @param[in]   rowsB                 No. of rows of Input matrix B. It should
 *                                    be a positive integer and a multiple
 *                                    of 64.
 * @param[in]   colsB                 No. of columns of Input matrix B. It
 *                                    should be a positive integer and a
 *                                    multiple of 8.
 * @param[in]   colIndexList          An array of column indices to be selected
 *                                    from prepared B. All indices of the array
 *                                    should be valid
 *                                    i.e. 0 <= colIndexList[N] < colsB
 *                                    where N = 0, 1 ....(`sizeColIndexList`-1)
 * @param[in]   sizeColIndexList      Size of the `colIndexList` array. It
 *                                    should be a positive integer and a
 *                                    multiple of 8.
 * @param[out]  output                An array representing the selected columns
 *                                    of prepared B.
 *                                    Size = `rowsB` * `sizeColIndexList`.
 *
 * This function implements the intrinsic:
 *   int8_select_columns_of_b(inputMatrixBPrepared: i32, rowsB: i32, colsB: i32,
 * colIndexList: i32, sizeColIndexList: i32, output: i32) which implements the
 * function: int8_select_columns_of_b(const int8_t* inputMatrixBPrepared,
 * uint32_t rowsB, uint32_t colsB, const uint32_t* colIndexList, const uint32_t
 * sizeColIndexList, int8_t* output)
 */
int32_t IntrI8SelectColumnsOfB(wasm::Instance* instance,
                               uint32_t inputMatrixBPrepared, uint32_t rowsB,
                               uint32_t colsB, uint32_t colIndexList,
                               uint32_t sizeColIndexList, uint32_t output,
                               uint8_t* memBase);

}  // namespace intgemm
}  // namespace js

#endif  // intgemm_IntegerGemmIntrinsic_h
