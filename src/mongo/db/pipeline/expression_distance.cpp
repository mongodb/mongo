#if !defined(DISTANCE_EXPRESSION_NOT_BSON) && !defined(USE_AVX2) && !defined(USE_AVX512)

#include "mongo/db/pipeline/expression_distance.h"

namespace mongo {

REGISTER_EXPRESSION(euclidean, ExpressionEuclidean::parse)
REGISTER_EXPRESSION(cossim, ExpressionCosineSimilarity::parse)
REGISTER_EXPRESSION(chi2, ExpressionChi2::parse)
REGISTER_EXPRESSION(squared_euclidean, ExpressionSquaredEuclidean::parse)
REGISTER_EXPRESSION(manhattan, ExpressionManhattan::parse)
REGISTER_EXPRESSION(no_op, ExpressionNoOp::parse)

/* ------------------------- ExpressionEuclideanBin ----------------------------- */

Value ExpressionEuclidean::evaluateImpl(const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    float r = 0.0;
    for (size_t i = 0; i < p_uiSize; ++i, ++p_pData1, ++p_pData2) {
        float diff = *p_pData1 - *p_pData2;
        r += diff * diff;
    }

    return Value(double(std::sqrt(r)));
}

/* ------------------------- ExpressionCosineSimilarityBin ----------------------------- */

Value ExpressionCosineSimilarity::evaluateImpl(const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    float dot = 0.0;
    float norm_a = 0.0;
    float norm_b = 0.0;

    for (size_t i = 0; i < p_uiSize; ++i, ++p_pData1, ++p_pData2) {
        float a = *p_pData1;
        float b = *p_pData2;

        dot += a * b;
        norm_a += a * a;
        norm_b += b * b;
    }

    float result = 1 - ( dot / ( (std::sqrt(norm_a*norm_b) + FLT_MIN) ));
    return Value(double(result));
}

/* ------------------------- ExpressionChi2Bin ----------------------------- */

Value ExpressionChi2::evaluateImpl(const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    float r = 0.0f;
    for (size_t i = 0; i < p_uiSize; ++i, ++p_pData1, ++p_pData2) {
        float a = *p_pData1;
        float b = *p_pData2;
	    float t = a + b;
        float diff = a - b;

        r += (diff * diff) / ( t + FLT_MIN);
    }

    return Value(double(r));
}

/* ------------------------- ExpressionSquaredEuclideanBin ----------------------------- */

Value ExpressionSquaredEuclidean::evaluateImpl(const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    float r = 0.f;
    for (size_t i = 0; i < p_uiSize; ++i, ++p_pData1, ++p_pData2) {
        float diff = *p_pData1 - *p_pData2;
        r += diff * diff;
    }

    return Value(double(r));
}

/* ------------------------- ExpressionManhattanBin ----------------------------- */

Value ExpressionManhattan::evaluateImpl(const float* p_pData1, const float* p_pData2, const size_t p_uiSize) const { 
    float r = 0.0;
    
    for (size_t i = 0; i < p_uiSize; ++i, ++p_pData1, ++p_pData2) {
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