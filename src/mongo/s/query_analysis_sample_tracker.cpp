/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/s/query_analysis_sample_tracker.h"

#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/is_mongos.h"

namespace mongo {
namespace analyze_shard_key {
namespace {

const auto getQueryAnalysisSampleTracker =
    ServiceContext::declareDecoration<QueryAnalysisSampleTracker>();

}  // namespace

QueryAnalysisSampleTracker& QueryAnalysisSampleTracker::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisSampleTracker& QueryAnalysisSampleTracker::get(ServiceContext* serviceContext) {
    return getQueryAnalysisSampleTracker(serviceContext);
}

void QueryAnalysisSampleTracker::refreshConfigurations(
    const std::vector<CollectionQueryAnalyzerConfiguration>& configurations) {
    stdx::lock_guard<Latch> lk(_mutex);
    std::map<NamespaceString, std::shared_ptr<QueryAnalysisSampleTracker::CollectionSampleTracker>>
        newTrackers;

    for (const auto& configuration : configurations) {
        auto it = _trackers.find(configuration.getNs());
        if (it == _trackers.end() ||
            it->second->getCollUuid() != configuration.getCollectionUuid()) {
            newTrackers.emplace(std::make_pair(
                configuration.getNs(),
                std::make_shared<CollectionSampleTracker>(configuration.getNs(),
                                                          configuration.getCollectionUuid(),
                                                          configuration.getSampleRate(),
                                                          configuration.getStartTime())));
        } else {
            it->second->setSampleRate(configuration.getSampleRate());
            it->second->setStartTime(configuration.getStartTime());
            newTrackers.emplace(std::make_pair(configuration.getNs(), it->second));
        }
        _sampledNamespaces.insert(configuration.getNs());
    }
    _trackers = std::move(newTrackers);
}

void QueryAnalysisSampleTracker::incrementReads(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const boost::optional<UUID>& collUuid,
                                                boost::optional<int64_t> size) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto counters = _getOrCreateCollectionSampleTracker(lk, opCtx, nss, collUuid);
    counters->incrementReads(size);
    ++_totalSampledReadsCount;
    if (size) {
        _totalSampledReadsBytes += *size;
    }
}

void QueryAnalysisSampleTracker::incrementWrites(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const boost::optional<UUID>& collUuid,
                                                 boost::optional<int64_t> size) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto counters = _getOrCreateCollectionSampleTracker(lk, opCtx, nss, collUuid);
    counters->incrementWrites(size);
    ++_totalSampledWritesCount;
    if (size) {
        _totalSampledWritesBytes += *size;
    }
}

std::shared_ptr<QueryAnalysisSampleTracker::CollectionSampleTracker>
QueryAnalysisSampleTracker::_getOrCreateCollectionSampleTracker(
    WithLock,
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& collUuid) {
    auto it = _trackers.find(nss);
    if (it == _trackers.end()) {
        // Do not create a new set of counters without collUuid specified:
        invariant(collUuid);
        auto startTime = opCtx->getServiceContext()->getFastClockSource()->now();
        it = _trackers
                 .emplace(std::make_pair(
                     nss,
                     std::make_shared<QueryAnalysisSampleTracker::CollectionSampleTracker>(
                         nss, *collUuid, 0 /* sampleRate */, startTime)))
                 .first;
        _sampledNamespaces.insert(nss);
    }
    return it->second;
}

void QueryAnalysisSampleTracker::reportForCurrentOp(std::vector<BSONObj>* ops) const {
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto const& it : _trackers) {
        ops->push_back(it.second->reportForCurrentOp());
    }
}

BSONObj QueryAnalysisSampleTracker::CollectionSampleTracker::reportForCurrentOp() const {
    CollectionSampleCountersCurrentOp report;
    report.setNs(_nss);
    report.setCollUuid(_collUuid);
    report.setSampledReadsCount(_sampledReadsCount);
    report.setSampledWritesCount(_sampledWritesCount);
    if (!isMongos()) {
        report.setSampledReadsBytes(_sampledReadsBytes);
        report.setSampledWritesBytes(_sampledWritesBytes);
    }
    if (isMongos() || serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        report.setSampleRate(_sampleRate);
    }
    report.setStartTime(_startTime);

    return report.toBSON();
}

BSONObj QueryAnalysisSampleTracker::reportForServerStatus() const {
    QueryAnalysisServerStatus res;
    res.setActiveCollections(static_cast<int64_t>(_trackers.size()));
    res.setTotalCollections(static_cast<int64_t>(_sampledNamespaces.size()));
    res.setTotalSampledReadsCount(_totalSampledReadsCount);
    res.setTotalSampledWritesCount(_totalSampledWritesCount);
    if (!isMongos()) {
        res.setTotalSampledReadsBytes(_totalSampledReadsBytes);
        res.setTotalSampledWritesBytes(_totalSampledWritesBytes);
    }
    return res.toBSON();
}

}  // namespace analyze_shard_key
}  // namespace mongo
