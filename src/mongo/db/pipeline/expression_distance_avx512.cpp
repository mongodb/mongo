#if !defined(DISTANCE_EXPRESSION_NOT_BSON) && defined(USE_AVX512)

#include "mongo/db/pipeline/expression_distance.h"
#include <immintrin.h>

namespace mongo {

REGISTER_EXPRESSION(euclidean, ExpressionEuclidean::parse)
REGISTER_EXPRESSION(cossim, ExpressionCosineSimilarity::parse)
REGISTER_EXPRESSION(chi2, ExpressionChi2::parse)
REGISTER_EXPRESSION(squared_euclidean, ExpressionSquaredEuclidean::parse)
REGISTER_EXPRESSION(manhattan, ExpressionManhattan::parse)
REGISTER_EXPRESSION(no_op, ExpressionNoOp::parse)

/* ------------------------- ExpressionEuclidean ----------------------------- */

/**
 * @brief Union used to access the __m512 individual values
 */
union U512f
{
    __m512 v;   ///< AVX vector
    float a[16]; ///< Equivalent float array
};

Value ExpressionEuclidean::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U512f sPackVal;
    __m512& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm512_set1_ps( 0.0f );

    const size_t uiPackSize = sizeof(__m512) / sizeof(float);
    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m512 packVec1 = _mm512_loadu_ps( p_pData1 );
        const __m512 packVec2 = _mm512_loadu_ps( p_pData2 );
        const __m512 packDiff = _mm512_sub_ps(packVec1, packVec2);
        packVal = _mm512_fmadd_ps( packDiff, packDiff, packVal );
    }

    float r = 0.0f;
    for (size_t i = 0; i < uiPackSize; ++i, ++pPackVal) {
        r += *pPackVal;
    }

    for (size_t i = 0; i < p_uiSize % uiPackSize; ++i, ++p_pData1, ++p_pData2) {
        float fDiff = *p_pData1 - *p_pData2;
        r += fDiff * fDiff;
    }

    return Value(double(std::sqrt(r)));
}

/* ------------------------- ExpressionCosineSimilarity ----------------------------- */

Value ExpressionCosineSimilarity::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 

    U512f sPackDot;
    __m512& packDot = sPackDot.v;
    const float* pPackDot = sPackDot.a;
    packDot = _mm512_set1_ps( 0.0f );

    U512f sPackNorm_a;
    __m512& packNorm_a = sPackNorm_a.v;
    const float* pPackNorm_a = sPackNorm_a.a;
    packNorm_a = _mm512_set1_ps( 0.0f );

    U512f sPackNorm_b;
    __m512& packNorm_b = sPackNorm_b.v;
    const float* pPackNorm_b = sPackNorm_b.a;
    packNorm_b = _mm512_set1_ps( 0.0f );

    const size_t uiPackSize = sizeof(__m512) / sizeof(float);

    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m512 packVec1 = _mm512_loadu_ps( p_pData1 );
        const __m512 packVec2 = _mm512_loadu_ps( p_pData2 );

        packDot = _mm512_fmadd_ps( packVec1, packVec2, packDot );
        packNorm_a = _mm512_fmadd_ps( packVec1, packVec1, packNorm_a );
        packNorm_b = _mm512_fmadd_ps( packVec2, packVec2, packNorm_b );
    }

    float dot = 0.f;
    float norm_a = 0.f;
    float norm_b = 0.f;
    for (size_t i = 0; i < uiPackSize; ++i, ++pPackDot, ++pPackNorm_a, ++pPackNorm_b) {
        dot += *pPackDot;
        norm_a += *pPackNorm_a;
        norm_b += *pPackNorm_b;
    }

    for (size_t i = 0; i < p_uiSize % uiPackSize; ++i, ++p_pData1, ++p_pData2) {
        float a = *p_pData1;
        float b = *p_pData2;

        dot += a * b;
        norm_a += a * a;
        norm_b += b * b;
    }

    float result = 1 - ( dot / ( (std::sqrt(norm_a*norm_b) + FLT_MIN) ));
    return Value(double(result));
}

/* ------------------------- ExpressionChi2 ----------------------------- */

Value ExpressionChi2::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U512f sPackVal;
    __m512& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm512_set1_ps( 0.0f );

    const __m512 packDBL_MIN =  _mm512_set1_ps( FLT_MIN );

    const size_t uiPackSize = sizeof(__m512) / sizeof(float);

    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m512 packVec1 = _mm512_loadu_ps( p_pData1 );
        const __m512 packVec2 = _mm512_loadu_ps( p_pData2 );

        const __m512 packAdd = _mm512_add_ps(packVec1, packVec2);
        const __m512 packSub = _mm512_sub_ps(packVec1, packVec2);

        packVal = _mm512_add_ps(
            packVal,
            _mm512_div_ps(
                _mm512_mul_ps(packSub, packSub),
                _mm512_add_ps(packAdd, packDBL_MIN)));
    }

    float r = 0.0f;
    for (size_t i = 0; i < uiPackSize; ++i, ++pPackVal) {
        r += *pPackVal;
    }

    for (size_t i = 0; i < p_uiSize % uiPackSize; ++i, ++p_pData1, ++p_pData2) {
        float a = *p_pData1;
        float b = *p_pData2;
	    float t = a + b;
        float diff = a - b;

        r += (diff * diff) / ( t + FLT_MIN);
    }

    return Value(double(r));
}

/* ------------------------- ExpressionSquaredEuclidean ----------------------------- */

Value ExpressionSquaredEuclidean::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U512f sPackVal;
    __m512& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm512_set1_ps( 0.0f );

    const size_t uiPackSize = sizeof(__m512) / sizeof(float);
    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m512 packVec1 = _mm512_loadu_ps( p_pData1 );
        const __m512 packVec2 = _mm512_loadu_ps( p_pData2 );
        const __m512 packDiff = _mm512_sub_ps(packVec1, packVec2);
        packVal = _mm512_fmadd_ps( packDiff, packDiff, packVal );
    }

    float r = 0.0f;
    for (size_t i = 0; i < uiPackSize; ++i, ++pPackVal) {
        r += *pPackVal;
    }

    for (size_t i = 0; i < p_uiSize % uiPackSize; ++i, ++p_pData1, ++p_pData2) {
        float diff = *p_pData1 - *p_pData2;
        r += diff * diff;
    }

    return Value(double(r));
}

/* ------------------------- ExpressionManhattanBin ----------------------------- */

template <int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7, int i8, int i9, int i10, int i11, int i12, int i13, int i14, int i15>
static inline __m512 constant16f() {
    static const union {
        int     i[16];
        __m512  ymm;
    } u = {{i0,i1,i2,i3,i4,i5,i6,i7,i8,i9,i10,i11,i12,i13,i14,i15}};
    return u.ymm;
}

Value ExpressionManhattan::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U512f sPackVal;
    __m512& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm512_set1_ps( 0.0f );

    __m512 mask = constant16f<0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF> ();

    const size_t uiPackSize = sizeof(__m512) / sizeof(float);

    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m512 packVec1 = _mm512_loadu_ps( p_pData1 );
        const __m512 packVec2 = _mm512_loadu_ps( p_pData2 );
        const __m512 packABSDiff = _mm512_and_ps(_mm512_sub_ps(packVec1, packVec2), mask);
        packVal = _mm512_add_ps( packABSDiff, packVal );
    }

    float r = 0.0f;
    for (size_t i = 0; i < uiPackSize; ++i, ++pPackVal) {
        r += *pPackVal;
    }

    for (size_t i = 0; i < p_uiSize % uiPackSize; ++i, ++p_pData1, ++p_pData2) {
        r += std::fabs( *p_pData1 - *p_pData2 );
    }

    return Value( double(r) );
}

/* ------------------------- ExpressionNoOp ----------------------------- */

inline Value ExpressionNoOp::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    return Value( 0.0 );
}

}

#endif
