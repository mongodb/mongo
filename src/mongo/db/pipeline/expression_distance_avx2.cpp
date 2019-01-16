#if !defined(DISTANCE_EXPRESSION_NOT_BSON) && defined(USE_AVX2) && !defined(USE_AVX512)

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
 * @brief Union used to access the __m256 individual values
 */
union U256f
{
    __m256 v;   ///< AVX vector
    float a[8]; ///< Equivalent float array
};

Value ExpressionEuclidean::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U256f sPackVal;
    __m256& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm256_set1_ps( 0.0f );

    const size_t uiPackSize = sizeof(__m256) / sizeof(float);
    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m256 packVec1 = _mm256_loadu_ps( p_pData1 );
        const __m256 packVec2 = _mm256_loadu_ps( p_pData2 );
        const __m256 packDiff = _mm256_sub_ps(packVec1, packVec2);
        packVal = _mm256_fmadd_ps( packDiff, packDiff, packVal );
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

    U256f sPackDot;
    __m256& packDot = sPackDot.v;
    const float* pPackDot = sPackDot.a;
    packDot = _mm256_set1_ps( 0.0f );

    U256f sPackNorm_a;
    __m256& packNorm_a = sPackNorm_a.v;
    const float* pPackNorm_a = sPackNorm_a.a;
    packNorm_a = _mm256_set1_ps( 0.0f );

    U256f sPackNorm_b;
    __m256& packNorm_b = sPackNorm_b.v;
    const float* pPackNorm_b = sPackNorm_b.a;
    packNorm_b = _mm256_set1_ps( 0.0f );

    const size_t uiPackSize = sizeof(__m256) / sizeof(float);

    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m256 packVec1 = _mm256_loadu_ps( p_pData1 );
        const __m256 packVec2 = _mm256_loadu_ps( p_pData2 );

        packDot = _mm256_fmadd_ps( packVec1, packVec2, packDot );
        packNorm_a = _mm256_fmadd_ps( packVec1, packVec1, packNorm_a );
        packNorm_b = _mm256_fmadd_ps( packVec2, packVec2, packNorm_b );
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

    float result = 1 - ( dot / ( (std::sqrt(norm_a * norm_b) + FLT_MIN) ));
    return Value(double(result));
}

/* ------------------------- ExpressionChi2 ----------------------------- */

Value ExpressionChi2::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U256f sPackVal;
    __m256& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm256_set1_ps( 0.0f );

    const __m256 packDBL_MIN =  _mm256_set1_ps( FLT_MIN );

    const size_t uiPackSize = sizeof(__m256) / sizeof(float);

    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m256 packVec1 = _mm256_loadu_ps( p_pData1 );
        const __m256 packVec2 = _mm256_loadu_ps( p_pData2 );

        const __m256 packAdd = _mm256_add_ps(packVec1, packVec2);
        const __m256 packSub = _mm256_sub_ps(packVec1, packVec2);

        packVal = _mm256_add_ps(
            packVal,
            _mm256_div_ps(
                _mm256_mul_ps(packSub, packSub),
                _mm256_add_ps(packAdd, packDBL_MIN)));
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
    U256f sPackVal;
    __m256& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm256_set1_ps( 0.0f );

    const size_t uiPackSize = sizeof(__m256) / sizeof(float);
    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m256 packVec1 = _mm256_loadu_ps( p_pData1 );
        const __m256 packVec2 = _mm256_loadu_ps( p_pData2 );
        const __m256 packDiff = _mm256_sub_ps(packVec1, packVec2);
        packVal = _mm256_fmadd_ps( packDiff, packDiff, packVal );
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

template <int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7>
static inline __m256 constant8f() {
    static const union {
        int     i[8];
        __m256  ymm;
    } u = {{i0,i1,i2,i3,i4,i5,i6,i7}};
    return u.ymm;
}

Value ExpressionManhattan::evaluateImpl(
    const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    U256f sPackVal;
    __m256& packVal = sPackVal.v;
    const float* pPackVal = sPackVal.a;
    packVal = _mm256_set1_ps( 0.0f );

    __m256 mask = constant8f<0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF> ();

    const size_t uiPackSize = sizeof(__m256) / sizeof(float);

    for (size_t i = 0; i < p_uiSize / uiPackSize; ++i, p_pData1 += uiPackSize, p_pData2 += uiPackSize) {
        const __m256 packVec1 = _mm256_loadu_ps( p_pData1 );
        const __m256 packVec2 = _mm256_loadu_ps( p_pData2 );
        const __m256 packABSDiff = _mm256_and_ps(_mm256_sub_ps(packVec1, packVec2), mask);
        packVal = _mm256_add_ps( packABSDiff, packVal );
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