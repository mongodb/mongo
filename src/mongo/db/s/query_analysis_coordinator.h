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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {
namespace analyze_shard_key {

/**
 * Keeps track of all samplers in the cluster and assigns sample rates to each sampler based on its
 * view of the query distribution across the samplers.
 *
 * On a sharded cluster, a sampler is any mongos or shardsvr mongod (that has acted as a
 * router) in the cluster, and the coordinator is the config server's primary mongod. On a
 * standalone replica set, a sampler is any mongod in the set and the coordinator is the primary
 * mongod.
 */
class QueryAnalysisCoordinator : public ReplicaSetAwareService<QueryAnalysisCoordinator> {
public:
    using CollectionQueryAnalyzerConfigurationMap =
        stdx::unordered_map<UUID, CollectionQueryAnalyzerConfiguration, UUID::Hash>;
    /**
     * Stores the last ping time and the last exponential moving average number of queries executed
     * per second for a sampler.
     */
    class Sampler {
    public:
        Sampler(std::string name, Date_t lastPingTime) : _name(name), _lastPingTime(lastPingTime){};

        std::string getName() const {
            return _name;
        }

        Date_t getLastPingTime() const {
            return _lastPingTime;
        }

        boost::optional<double> getLastNumQueriesExecutedPerSecond() const {
            return _lastNumQueriesExecutedPerSecond;
        }

        void setLastPingTime(Date_t pingTime);

        void setLastNumQueriesExecutedPerSecond(double numQueries);
        void resetLastNumQueriesExecutedPerSecond();

    private:
        std::string _name;
        Date_t _lastPingTime;
        boost::optional<double> _lastNumQueriesExecutedPerSecond;
    };

    QueryAnalysisCoordinator() = default;

    /**
     * Obtains the service-wide QueryAnalysisCoordinator instance.
     */
    static QueryAnalysisCoordinator* get(OperationContext* opCtx);
    static QueryAnalysisCoordinator* get(ServiceContext* serviceContext);

    void onStartup(OperationContext* opCtx) override final;

    void onStepUpBegin(OperationContext* opCtx, long long term) override final;

    /**
     * Creates, updates and deletes the configuration for the collection with the given
     * config.queryAnalyzers document.
     */
    void onConfigurationInsert(const QueryAnalyzerDocument& doc);
    void onConfigurationUpdate(const QueryAnalyzerDocument& doc);
    void onConfigurationDelete(const QueryAnalyzerDocument& doc);

    /**
     * On a sharded cluster, creates, updates and deletes the sampler for the mongos with the given
     * config.mongos document.
     */
    void onSamplerInsert(const MongosType& doc);
    void onSamplerUpdate(const MongosType& doc);
    void onSamplerDelete(const MongosType& doc);

    /**
     * Given the average number of queries that a sampler executes, returns the new query analyzer
     * configurations for the sampler.
     */
    std::vector<CollectionQueryAnalyzerConfiguration> getNewConfigurationsForSampler(
        OperationContext* opCtx, StringData samplerName, double numQueriesExecutedPerSecond);


    CollectionQueryAnalyzerConfigurationMap getConfigurationsForTest() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _configurations;
    }

    void clearConfigurationsForTest() {
        stdx::lock_guard<Latch> lk(_mutex);
        _configurations.clear();
    }

    StringMap<Sampler> getSamplersForTest() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _samplers;
    }

    void clearSamplersForTest() {
        stdx::lock_guard<Latch> lk(_mutex);
        _samplers.clear();
    }

private:
    bool shouldRegisterReplicaSetAwareService() const override final;

    /**
     * On a standalone replica set, creates, updates and removes samplers based on the current
     * replica set configuration.
     */
    void onSetCurrentConfig(OperationContext* opCtx) override final;

    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {}

    void onShutdown() override final {}

    void onStepUpComplete(OperationContext* opCtx, long long term) override final {}

    void onStepDown() override final {}

    void onBecomeArbiter() override final {}

    inline std::string getServiceName() const override final {
        return "QueryAnalysisCoordinator";
    }

    /**
     * Returns the minimum last ping time for a sampler to be considered as active.
     */
    Date_t _getMinLastPingTime();

    mutable Mutex _mutex = MONGO_MAKE_LATCH("QueryAnalysisCoordinator::_mutex");

    CollectionQueryAnalyzerConfigurationMap _configurations;
    StringMap<Sampler> _samplers;
};

}  // namespace analyze_shard_key
}  // namespace mongo
