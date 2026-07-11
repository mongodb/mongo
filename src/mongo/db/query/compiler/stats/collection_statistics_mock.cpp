// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/collection_statistics_mock.h"

#include <map>
#include <utility>

namespace mongo::stats {

CollectionStatisticsMock::CollectionStatisticsMock(double cardinality)
    : _cardinality{cardinality}, _histograms{} {};

double CollectionStatisticsMock::getCardinality() const {
    return _cardinality;
}

void CollectionStatisticsMock::addHistogram(const std::string& path,
                                            std::shared_ptr<const CEHistogram> histogram) const {
    _histograms[path] = histogram;
}

const CEHistogram* CollectionStatisticsMock::getHistogram(const std::string& path) const {
    if (auto mapIt = _histograms.find(path); mapIt != _histograms.end()) {
        return mapIt->second.get();
    }
    return nullptr;
}

}  // namespace mongo::stats
