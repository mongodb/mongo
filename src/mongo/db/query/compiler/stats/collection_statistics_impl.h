// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/collection_statistics.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string>

namespace mongo::stats {

using Histograms = std::map<std::string, std::shared_ptr<const CEHistogram>>;

class CollectionStatisticsImpl : public CollectionStatistics {
public:
    CollectionStatisticsImpl(double cardinality, const NamespaceString& nss);

    /**
     * Returns the cardinality of the given collection.
     */
    double getCardinality() const override;

    /**
     * Returns the histogram for the given field path, or nullptr if none exists.
     */
    const CEHistogram* getHistogram(const std::string& path) const override;

    /**
     * Adds a histogram along the given path.
     */
    void addHistogram(const std::string& path,
                      std::shared_ptr<const CEHistogram> histogram) const override;

    ~CollectionStatisticsImpl() override = default;

private:
    double _cardinality;
    mutable Histograms _histograms;
    const NamespaceString _nss;
};

}  // namespace mongo::stats
