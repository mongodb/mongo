// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/query/compiler/stats/stats_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo::stats {

CollectionStatisticsImpl::CollectionStatisticsImpl(double cardinality, const NamespaceString& nss)
    : _cardinality{cardinality}, _histograms{}, _nss{nss} {};

double CollectionStatisticsImpl::getCardinality() const {
    return _cardinality;
}

void CollectionStatisticsImpl::addHistogram(const std::string& path,
                                            std::shared_ptr<const CEHistogram> histogram) const {
    _histograms[path] = histogram;
}

const CEHistogram* CollectionStatisticsImpl::getHistogram(const std::string& path) const {
    if (auto mapIt = _histograms.find(path); mapIt != _histograms.end()) {
        return mapIt->second.get();
    } else {
        uassert(8423368, "no current client", Client::getCurrent());
        auto opCtx = Client::getCurrent()->getOperationContext();
        uassert(8423367, "no operation context", opCtx);
        StatsCatalog& statsCatalog = StatsCatalog::get(opCtx);
        const auto swHistogram = statsCatalog.getHistogram(opCtx, _nss, path);
        if (!swHistogram.isOK()) {
            if (swHistogram != ErrorCodes::NamespaceNotFound) {
                uasserted(swHistogram.getStatus().code(),
                          str::stream()
                              << "Error getting histograms for path " << _nss.toStringForErrorMsg()
                              << " : " << path << swHistogram.getStatus().reason());
            }
            return nullptr;
        }
        const auto histogram = std::move(swHistogram.getValue());
        addHistogram(path, histogram);
        return histogram.get();
    }
}

}  // namespace mongo::stats
