/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/query_analysis_coordinator.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

const auto getQueryAnalysisCoordinator =
    ServiceContext::declareDecoration<QueryAnalysisCoordinator>();

const ReplicaSetAwareServiceRegistry::Registerer<QueryAnalysisCoordinator>
    queryAnalysisCoordinatorServiceServiceRegisterer("QueryAnalysisCoordinator");

}  // namespace


QueryAnalysisCoordinator* QueryAnalysisCoordinator::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisCoordinator* QueryAnalysisCoordinator::get(ServiceContext* serviceContext) {
    return &getQueryAnalysisCoordinator(serviceContext);
}

bool QueryAnalysisCoordinator::shouldRegisterReplicaSetAwareService() const {
    // This is invoked when the Register above is constructed which is before FCV is set so we need
    // to ignore FCV when checking if the feature flag is enabled.
    return analyze_shard_key::gFeatureFlagAnalyzeShardKey.isEnabledAndIgnoreFCV() &&
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
}

void QueryAnalysisCoordinator::onConfigurationInsert(const BSONObj& doc) {
    stdx::lock_guard<Latch> lk(_mutex);

    auto analyzerDoc =
        QueryAnalyzerDocument::parse(IDLParserContext("QueryAnalysisCoordinator"), doc);
    if (analyzerDoc.getMode() == QueryAnalyzerModeEnum::kOff) {
        // Do not create an entry for it if the mode is "off".
        return;
    }

    auto configuration = CollectionQueryAnalyzerConfiguration{
        analyzerDoc.getNs(), analyzerDoc.getCollectionUuid(), *analyzerDoc.getSampleRate()};
    _configurations.emplace(analyzerDoc.getCollectionUuid(), std::move(configuration));
}

void QueryAnalysisCoordinator::onConfigurationUpdate(const BSONObj& doc) {
    stdx::lock_guard<Latch> lk(_mutex);

    auto analyzerDoc =
        QueryAnalyzerDocument::parse(IDLParserContext("QueryAnalysisCoordinator"), doc);
    if (analyzerDoc.getMode() == QueryAnalyzerModeEnum::kOff) {
        // Remove the entry for it if the mode has been set to "off".
        _configurations.erase(analyzerDoc.getCollectionUuid());
    } else {
        auto it = _configurations.find(analyzerDoc.getCollectionUuid());
        if (it == _configurations.end()) {
            auto configuration = CollectionQueryAnalyzerConfiguration{
                analyzerDoc.getNs(), analyzerDoc.getCollectionUuid(), *analyzerDoc.getSampleRate()};
            _configurations.emplace(analyzerDoc.getCollectionUuid(), std::move(configuration));
        } else {
            it->second.setSampleRate(*analyzerDoc.getSampleRate());
        }
    }
}

void QueryAnalysisCoordinator::onConfigurationDelete(const BSONObj& doc) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto analyzerDoc =
        QueryAnalyzerDocument::parse(IDLParserContext("QueryAnalysisCoordinator"), doc);
    _configurations.erase(analyzerDoc.getCollectionUuid());
}

void QueryAnalysisCoordinator::onStartup(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);

    DBDirectClient client(opCtx);

    {
        invariant(_configurations.empty());
        FindCommandRequest findRequest{NamespaceString::kConfigQueryAnalyzersNamespace};
        findRequest.setFilter(BSON(QueryAnalyzerDocument::kModeFieldName << BSON("$ne"
                                                                                 << "off")));
        auto cursor = client.find(std::move(findRequest));
        while (cursor->more()) {
            auto analyzerDoc = QueryAnalyzerDocument::parse(
                IDLParserContext("QueryAnalysisCoordinator"), cursor->next());
            invariant(analyzerDoc.getMode() != QueryAnalyzerModeEnum::kOff);
            auto configuration = CollectionQueryAnalyzerConfiguration{
                analyzerDoc.getNs(), analyzerDoc.getCollectionUuid(), *analyzerDoc.getSampleRate()};
            auto [_, inserted] =
                _configurations.emplace(analyzerDoc.getCollectionUuid(), std::move(configuration));
            invariant(inserted);
        }
    }
}

}  // namespace analyze_shard_key
}  // namespace mongo
