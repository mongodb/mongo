// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/collection_statistics.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo::stats {

class CollectionStatisticsMock : public CollectionStatistics {
public:
    CollectionStatisticsMock(double cardinality);

    /**
     * Returns the cardinality of the given collection.
     */
    double getCardinality() const override;

    /**
     * Adds a histogram along the given path.
     */
    void addHistogram(const std::string& path,
                      std::shared_ptr<const CEHistogram> histogram) const override;

    /**
     * Returns the histogram for the given field path, or nullptr if none exists.
     */
    const CEHistogram* getHistogram(const std::string& path) const override;

    ~CollectionStatisticsMock() override = default;

private:
    double _cardinality;
    mutable Histograms _histograms;
};

}  // namespace mongo::stats
