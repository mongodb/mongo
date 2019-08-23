#ifdef DISTANCE_EXPRESSION_NOT_BSON

#include "mongo/db/pipeline/expression_distance.h"

namespace mongo {

REGISTER_EXPRESSION(euclidean, ExpressionEuclidean::parse)
REGISTER_EXPRESSION(cossim, ExpressionCosineSimilarity::parse)
REGISTER_EXPRESSION(chi2, ExpressionChi2::parse)
REGISTER_EXPRESSION(squared_euclidean, ExpressionSquaredEuclidean::parse)
REGISTER_EXPRESSION(manhattan, ExpressionManhattan::parse)
REGISTER_EXPRESSION(no_op, ExpressionNoOp::parse)

/* ------------------------- ExpressionEuclidean ----------------------------- */

inline Value ExpressionEuclidean::evaluateImpl(
    const std::vector<Value>& vector1, const std::vector<Value>& vector2) const {
    double r = 0.0;

    auto it1 = vector1.cbegin();
    auto it2 = vector2.cbegin();

    for (size_t i = 0; i < vector1.size(); ++i, ++it1, ++it2) {
        double diff = it1->getDouble() - it2->getDouble();
        r += diff * diff;
    }

    return Value(std::sqrt(r));
}

/* ------------------------- ExpressionCosineSimilarity ----------------------------- */

inline Value ExpressionCosineSimilarity::evaluateImpl(
    const std::vector<Value>& vector1, const std::vector<Value>& vector2) const {

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    
    auto it1 = vector1.cbegin();
    auto it2 = vector2.cbegin();

    for (size_t i = 0; i < vector1.size(); ++i, ++it1, ++it2) {
        double a = it1->getDouble();
        double b = it2->getDouble();

        dot += a * b;
        norm_a += a * a;
        norm_b += b * b;
    }

    double result = 1 - ( dot / ( std::sqrt(norm_a*norm_b) + FLT_MIN) );
    return Value(result);
}

/* ------------------------- ExpressionChi2 ----------------------------- */

inline Value ExpressionChi2::evaluateImpl(
    const std::vector<Value>& vector1, const std::vector<Value>& vector2) const {

    double r = 0.0;
    auto it1 = vector1.cbegin();
    auto it2 = vector2.cbegin();

    for (size_t i = 0; i < vector1.size(); ++i, ++it1, ++it2) {
        double a = it1->getDouble();
        double b = it2->getDouble();
	    double t = a + b;
        double diff = a - b;

        r += (diff * diff) / ( t + FLT_MIN);
    }

    return Value(r);
}

/* ------------------------- ExpressionSquaredEuclidean ----------------------------- */

inline Value ExpressionSquaredEuclidean::evaluateImpl(
    const std::vector<Value>& vector1, const std::vector<Value>& vector2) const {
    double r = 0.0;

    auto it1 = vector1.cbegin();
    auto it2 = vector2.cbegin();

    for (size_t i = 0; i < vector1.size(); ++i, ++it1, ++it2) {
        double diff = it1->getDouble() - it2->getDouble();
        r += diff * diff;
    }

    return Value(r);
}

/* ------------------------- ExpressionManhattan ----------------------------- */

inline Value ExpressionManhattan::evaluateImpl(
    const std::vector<Value>& vector1, const std::vector<Value>& vector2) const {
    double r = 0.0;
    auto it1 = vector1.cbegin();
    auto it2 = vector2.cbegin();

    for (size_t i = 0; i < vector1.size(); ++i, ++it1, ++it2) {
        r += std::abs( it1->getDouble() - it2->getDouble() );
    }

    return Value( r );
}

/* ------------------------- ExpressionNoOp ----------------------------- */

inline Value ExpressionNoOp::evaluateImpl(
    const std::vector<Value>& vector1, const std::vector<Value>& vector2) const {
    return Value( 0.0 );
}
}

#endif
