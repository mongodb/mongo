// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/util/modules.h"

namespace mongo::stats {

using Histograms = std::map<std::string, std::shared_ptr<const CEHistogram>>;

class CollectionStatistics {
public:
    /**
     * Returns the cardinality of the given collection.
     */
    virtual double getCardinality() const = 0;

    /**
     * Returns the histogram for the given field path, or nullptr if none exists.
     */
    virtual const CEHistogram* getHistogram(const std::string& path) const = 0;

    /**
     * Adds a histogram along the given path.
     */
    virtual void addHistogram(const std::string& path,
                              std::shared_ptr<const CEHistogram> histogram) const = 0;

    virtual ~CollectionStatistics() = default;
};

}  // namespace mongo::stats
