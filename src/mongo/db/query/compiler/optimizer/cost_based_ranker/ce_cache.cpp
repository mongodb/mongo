// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_cache.h"

namespace mongo::cost_based_ranker {

template <bool EnableLogging>
CECache<EnableLogging>::CECache() {
    if constexpr (EnableLogging) {
        _logStream << std::fixed << std::setprecision(2);
    }
}

template <bool EnableLogging>
CECache<EnableLogging>::~CECache() {
    if constexpr (EnableLogging) {
        auto hits = _meHits + _ibHits;
        auto misses = _meMisses + _ibMisses;
        double cacheHitRatio =
            misses > 0 ? static_cast<double>(hits) / static_cast<double>(misses) : 0;
        log("CECache summary: ",
            "[MatchExpressions] cached: ",
            _exprCache.size(),
            ", hits: ",
            _meHits,
            ", misses: ",
            _meMisses,
            "; [IntervalBounds] cached: ",
            _intervalCache.size(),
            ", hits: ",
            _ibHits,
            ", misses: ",
            _ibMisses,
            "; [Total] hits: ",
            hits,
            ", misses: ",
            misses,
            "; Cache hit ratio: ",
            cacheHitRatio);
        std::cout << _logStream.str() << std::endl;
    }
}

// Explicit instantiation definitions
template class CECache<true>;
template class CECache<false>;
}  // namespace mongo::cost_based_ranker
