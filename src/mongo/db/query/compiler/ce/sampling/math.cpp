// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/math.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {
// The "Method-of-moments" estimator for NDV is defined as a solution 'D' for the equation
// d = D * (1 - e^(-n/D)), where 'd' is the sample's NDV and 'n' is the size of the sample. The
// Newton-Raphson method dictates that we can find a solution for that equation via iteration using
// a real-valued function 'f', its derivative, and an initial guess: x(n+1) = x(n) - f(x) / f'(x).

// For Method-of-moments NDV calculation, 'f' is f(x) = x * (1 - e^(-n/x)) - d...
double methodOfMoments(double x, double sampleSize, double distinctEstFromSample) {
    return (x * (1.0 - exp(-sampleSize / x))) - distinctEstFromSample;
}

// ... And its derivative f'(x) = 1 - e^(-n/x) * (1 + n/x).
double methodOfMomentsDerivative(double x, double sampleSize) {
    return 1.0 - (exp(-sampleSize / x) * (1.0 + sampleSize / x));
}

CardinalityEstimate newtonRaphsonNDV(size_t sampleNDV, size_t sampleSize) {
    tassert(11158508, "Sample NDV cannot be greater than the sample size", sampleNDV <= sampleSize);
    tassert(11158509, "Sample size must be greater than 1", sampleSize > 1);

    int MAX_ITER = 1000;
    double EPSILON = 0.001;
    int numIter = 0;

    // 'x' is our current guess for overall NDV. If it is equal to the sampleSize, it must be
    // adjusted down to ensure convergence.
    double x = (sampleNDV == sampleSize) ? sampleNDV - 1 : sampleNDV;
    for (double h = 1; abs(h) >= EPSILON && numIter <= MAX_ITER; ++numIter) {
        tassert(11158510,
                fmt::format("Current NDV estimate must be greater than zero with sampleNDV {} and "
                            "sampleSize {}",
                            sampleNDV,
                            sampleSize),
                x > 0);
        double deriv = methodOfMomentsDerivative(x, sampleSize);

        // Can't continue when the denominator is zero or very close to it. This can happen when
        // sampleNDV is very close to sampleSize.
        if (deriv <= EPSILON) {
            LOGV2_DEBUG(11158511,
                        5,
                        "Derivative approached zero",
                        "sampleNDV"_attr = sampleNDV,
                        "sampleSize"_attr = sampleSize,
                        "deriv"_attr = deriv);
            break;
        }

        h = methodOfMoments(x, sampleSize, sampleNDV) / deriv;
        if (h > x) {
            LOGV2_DEBUG(11228300,
                        5,
                        "Reached a divergent scenario",
                        "sampleNDV"_attr = sampleNDV,
                        "sampleSize"_attr = sampleSize,
                        "h"_attr = h,
                        "x"_attr = x,
                        "deriv"_attr = deriv);
            break;
        }

        // x(n+1) = x(n) - f(x) / f'(x)
        x = x - h;
    }

    if (numIter >= MAX_ITER) {
        LOGV2_DEBUG(11158512,
                    5,
                    "Hit max iterations",
                    "sampleNDV"_attr = sampleNDV,
                    "sampleSize"_attr = sampleSize);
    }

    // Sanity check: NDV is always at least the sample NDV.
    x = fmax(x, sampleNDV);
    return CardinalityEstimate{CardinalityType{(double)x}, EstimationSource::Sampling};
}
}  // namespace mongo::ce
