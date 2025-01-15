/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <cstdint>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/message.h"
#include "mongo/util/aligned.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * for storing operation counters
 * note: not thread safe.  ok with that for speed
 */
class OpCounters {
public:
    OpCounters() = default;

    void gotInserts(int n) {
        _checkWrap(&OpCounters::_insert, n);
    }
    void gotInsert() {
        _checkWrap(&OpCounters::_insert, 1);
    }
    void gotQuery() {
        _checkWrap(&OpCounters::_query, 1);
    }
    void gotUpdate() {
        _checkWrap(&OpCounters::_update, 1);
    }
    void gotDelete() {
        _checkWrap(&OpCounters::_delete, 1);
    }
    void gotGetMore() {
        _checkWrap(&OpCounters::_getmore, 1);
    }
    void gotCommand() {
        _checkWrap(&OpCounters::_command, 1);
    }

    void gotQueryDeprecated() {
        _checkWrap(&OpCounters::_queryDeprecated, 1);
    }

    void gotNestedAggregate() {
        _checkWrap(&OpCounters::_nestedAggregate, 1);
    }

    BSONObj getObj() const;

    // These opcounters record operations that would fail if we were fully enforcing our consistency
    // constraints in steady-state oplog application mode.
    void gotInsertOnExistingDoc() {
        _checkWrap(&OpCounters::_insertOnExistingDoc, 1);
    }
    void gotUpdateOnMissingDoc() {
        _checkWrap(&OpCounters::_updateOnMissingDoc, 1);
    }
    void gotDeleteWasEmpty() {
        _checkWrap(&OpCounters::_deleteWasEmpty, 1);
    }
    void gotDeleteFromMissingNamespace() {
        _checkWrap(&OpCounters::_deleteFromMissingNamespace, 1);
    }
    void gotAcceptableErrorInCommand() {
        _checkWrap(&OpCounters::_acceptableErrorInCommand, 1);
    }

    // thse are used by metrics things, do not remove
    const AtomicWord<long long>* getInsert() const {
        return &*_insert;
    }
    const AtomicWord<long long>* getQuery() const {
        return &*_query;
    }
    const AtomicWord<long long>* getUpdate() const {
        return &*_update;
    }
    const AtomicWord<long long>* getDelete() const {
        return &*_delete;
    }
    const AtomicWord<long long>* getGetMore() const {
        return &*_getmore;
    }
    const AtomicWord<long long>* getCommand() const {
        return &*_command;
    }
    const AtomicWord<long long>* getNestedAggregate() const {
        return &*_nestedAggregate;
    }
    const AtomicWord<long long>* getInsertOnExistingDoc() const {
        return &*_insertOnExistingDoc;
    }
    const AtomicWord<long long>* getUpdateOnMissingDoc() const {
        return &*_updateOnMissingDoc;
    }
    const AtomicWord<long long>* getDeleteWasEmpty() const {
        return &*_deleteWasEmpty;
    }
    const AtomicWord<long long>* getDeleteFromMissingNamespace() const {
        return &*_deleteFromMissingNamespace;
    }
    const AtomicWord<long long>* getAcceptableErrorInCommand() const {
        return &*_acceptableErrorInCommand;
    }

    // Reset all counters. To used for testing purposes only.
    void resetForTest() {
        _reset();
    }

private:
    // Reset all counters.
    void _reset();

    // Increment member `counter` by `n`, resetting all counters if it was > 2^60.
    void _checkWrap(CacheExclusive<AtomicWord<long long>> OpCounters::*counter, int n);

    CacheExclusive<AtomicWord<long long>> _insert;
    CacheExclusive<AtomicWord<long long>> _query;
    CacheExclusive<AtomicWord<long long>> _update;
    CacheExclusive<AtomicWord<long long>> _delete;
    CacheExclusive<AtomicWord<long long>> _getmore;
    CacheExclusive<AtomicWord<long long>> _command;
    CacheExclusive<AtomicWord<long long>> _nestedAggregate;

    CacheExclusive<AtomicWord<long long>> _insertOnExistingDoc;
    CacheExclusive<AtomicWord<long long>> _updateOnMissingDoc;
    CacheExclusive<AtomicWord<long long>> _deleteWasEmpty;
    CacheExclusive<AtomicWord<long long>> _deleteFromMissingNamespace;
    CacheExclusive<AtomicWord<long long>> _acceptableErrorInCommand;

    // Counter for the deprecated OP_QUERY opcode.
    CacheExclusive<AtomicWord<long long>> _queryDeprecated;
};

OpCounters& serviceOpCounters(ClusterRole role);
/** Convenience overload to fetch the serviceOpCounters for the role the opCtx is running under. */
inline OpCounters& serviceOpCounters(OperationContext* opCtx) {
    return serviceOpCounters(opCtx->getService()->role());
}

extern OpCounters replOpCounters;

class NetworkCounter {
public:
    enum class ConnectionType { kIngress = 1, kEgress = 2 };
    // Increment the counters for the number of bytes read directly off the wire
    void hitPhysicalIn(ConnectionType connectionType, long long bytes);
    void hitPhysicalOut(ConnectionType connectionType, long long bytes);

    // Increment the counters for the number of bytes passed out of the TransportLayer to the
    // server
    void hitLogicalIn(ConnectionType connectionType, long long bytes);
    void hitLogicalOut(ConnectionType connectionType, long long bytes);

    // Increment the counter for the number of slow dns resolution operations.
    void incrementNumSlowDNSOperations();

    // Increment the counter for the number of slow ssl handshake operations.
    void incrementNumSlowSSLOperations();

    // TFO Counters and Status;
    void acceptedTFOIngress();

    void setTFOKernelSetting(std::int64_t val) {
        _tfoKernelSetting = val;
    }

    void setTFOServerSupport(bool val) {
        _tfoKernelSupportServer = val;
    }

    void setTFOClientSupport(bool val) {
        _tfoKernelSupportClient = val;
    }

    void append(BSONObjBuilder& b);

private:
    CacheExclusive<AtomicWord<long long>> _ingressPhysicalBytesIn{0};
    CacheExclusive<AtomicWord<long long>> _ingressPhysicalBytesOut{0};

    CacheExclusive<AtomicWord<long long>> _egressPhysicalBytesIn{0};
    CacheExclusive<AtomicWord<long long>> _egressPhysicalBytesOut{0};

    // These two counters are always incremented at the same time, so
    // we place them on the same cache line. We use
    // CacheCombinedExclusive to ensure that they are combined within
    // the scope of a constructive interference region, and protected
    // from false sharing by padding out to destructive interference
    // size.
    struct Together {
        AtomicWord<long long> logicalBytesIn{0};
        AtomicWord<long long> requests{0};
    };

    CacheCombinedExclusive<Together> _ingressTogether{};
    CacheExclusive<AtomicWord<long long>> _ingressLogicalBytesOut{0};

    CacheCombinedExclusive<Together> _egressTogether{};
    CacheExclusive<AtomicWord<long long>> _egressLogicalBytesOut{0};

    CacheExclusive<AtomicWord<long long>> _numSlowDNSOperations{0};
    CacheExclusive<AtomicWord<long long>> _numSlowSSLOperations{0};

    // Counter of inbound connections at runtime.
    CacheExclusive<AtomicWord<std::int64_t>> _tfoAccepted{0};

    // TFO info determined at startup.
    std::int64_t _tfoKernelSetting{0};
    bool _tfoKernelSupportServer{false};
    bool _tfoKernelSupportClient{false};
};

extern NetworkCounter networkCounter;

class AuthCounter {
    struct MechanismData;

public:
    class MechanismCounterHandle {
    public:
        MechanismCounterHandle(MechanismData* data) : _data(data) {}

        void incSpeculativeAuthenticateReceived();
        void incSpeculativeAuthenticateSuccessful();

        void incAuthenticateReceived();
        void incAuthenticateSuccessful();

        void incClusterAuthenticateReceived();
        void incClusterAuthenticateSuccessful();

    private:
        MechanismData* _data;
    };

    MechanismCounterHandle getMechanismCounter(StringData mechanism);

    void incSaslSupportedMechanismsReceived();

    void incAuthenticationCumulativeTime(long long micros);

    void append(BSONObjBuilder*);

    void initializeMechanismMap(const std::vector<std::string>&);

private:
    struct MechanismData {
        struct {
            AtomicWord<long long> received;
            AtomicWord<long long> successful;
        } speculativeAuthenticate;
        struct {
            AtomicWord<long long> received;
            AtomicWord<long long> successful;
        } authenticate;
        struct {
            AtomicWord<long long> received;
            AtomicWord<long long> successful;
        } clusterAuthenticate;
    };
    using MechanismMap = std::map<std::string, MechanismData>;

    AtomicWord<long long> _saslSupportedMechanismsReceived;
    AtomicWord<long long> _authenticationCumulativeMicros;
    // Mechanism maps are initialized at startup to contain all
    // mechanisms known to authenticationMechanisms setParam.
    // After that they are kept to a fixed size.
    MechanismMap _mechanisms;
};
extern AuthCounter authCounter;

class AggStageCounters {
public:
    // Container for a stage count metric along with its corresponding counter.
    explicit AggStageCounters(std::string prefix) : _prefix{std::move(prefix)} {}

    void addMetric(const std::string& name) {
        _stages[name] = &*MetricBuilder<Counter64>(_prefix + name);
    }

    /** requires `name` be a metric previously added with `addMetric`. */
    void increment(StringData name, long long n = 1) {
        _stages.find(name)->second->incrementRelaxed(n);
    }

private:
    std::string _prefix;
    // Map of aggregation stages to the number of occurrences.
    StringMap<Counter64*> _stages;
};

extern AggStageCounters aggStageCounters;

class DotsAndDollarsFieldsCounters {
public:
    DotsAndDollarsFieldsCounters() = default;
    DotsAndDollarsFieldsCounters(DotsAndDollarsFieldsCounters&) = delete;
    DotsAndDollarsFieldsCounters& operator=(const DotsAndDollarsFieldsCounters&) = delete;

    void incrementForUpsert(bool didInsert) {
        if (didInsert) {
            inserts.incrementRelaxed();
        } else {
            updates.incrementRelaxed();
        }
    }

    Counter64& inserts = *MetricBuilder<Counter64>{"dotsAndDollarsFields.inserts"};
    Counter64& updates = *MetricBuilder<Counter64>{"dotsAndDollarsFields.updates"};
};

extern DotsAndDollarsFieldsCounters dotsAndDollarsFieldsCounters;

class QueryFrameworkCounters {
public:
    QueryFrameworkCounters() = default;
    QueryFrameworkCounters(QueryFrameworkCounters&) = delete;
    QueryFrameworkCounters& operator=(const QueryFrameworkCounters&) = delete;

    void incrementQueryEngineCounters(CurOp* curop) {
        auto& debug = curop->debug();
        const BSONObj& cmdObj = curop->opDescription();
        auto cmdName = cmdObj.firstElementFieldNameStringData();

        if (cmdName == "find") {
            switch (debug.queryFramework) {
                case PlanExecutor::QueryFramework::kClassicOnly:
                    classicFindQueryCounter.incrementRelaxed();
                    break;
                case PlanExecutor::QueryFramework::kSBEOnly:
                    sbeFindQueryCounter.incrementRelaxed();
                    break;
                default:
                    break;
            }
        } else if (cmdName == "aggregate") {
            switch (debug.queryFramework) {
                case PlanExecutor::QueryFramework::kClassicOnly:
                    classicOnlyAggregationCounter.incrementRelaxed();
                    break;
                case PlanExecutor::QueryFramework::kClassicHybrid:
                    classicHybridAggregationCounter.incrementRelaxed();
                    break;
                case PlanExecutor::QueryFramework::kSBEOnly:
                    sbeOnlyAggregationCounter.incrementRelaxed();
                    break;
                case PlanExecutor::QueryFramework::kSBEHybrid:
                    sbeHybridAggregationCounter.incrementRelaxed();
                    break;
                case PlanExecutor::QueryFramework::kUnknown:
                    break;
            }
        }
    }

    // Query counters that record whether a find query was fully or partially executed in SBE, or
    // fully executed using the classic engine. One of these will always be incremented during a
    // query.
    Counter64& sbeFindQueryCounter = *MetricBuilder<Counter64>{"query.queryFramework.find.sbe"};
    Counter64& classicFindQueryCounter =
        *MetricBuilder<Counter64>{"query.queryFramework.find.classic"};

    // Aggregation query counters that record whether an aggregation was fully or partially executed
    // in DocumentSource (an sbe/classic hybrid plan), or fully pushed down to the sbe/classic
    // layer. These are only incremented during aggregations.
    Counter64& sbeOnlyAggregationCounter =
        *MetricBuilder<Counter64>{"query.queryFramework.aggregate.sbeOnly"};
    Counter64& classicOnlyAggregationCounter =
        *MetricBuilder<Counter64>{"query.queryFramework.aggregate.classicOnly"};
    Counter64& sbeHybridAggregationCounter =
        *MetricBuilder<Counter64>{"query.queryFramework.aggregate.sbeHybrid"};
    Counter64& classicHybridAggregationCounter =
        *MetricBuilder<Counter64>{"query.queryFramework.aggregate.classicHybrid"};
};
extern QueryFrameworkCounters queryFrameworkCounters;

class FastPathQueryCounters {
public:
    void incrementIdHackQueryCounter() {
        idHackQueryCounter.increment();
    }

    void incrementExpressQueryCounter() {
        expressQueryCounter.increment();
    }

    // Counter for the number of queries planned using idHack fast planning.
    Counter64& idHackQueryCounter = *MetricBuilder<Counter64>{"query.planning.fastPath.idHack"};
    // Counter for the number of queries planned using express fast planning.
    Counter64& expressQueryCounter = *MetricBuilder<Counter64>{"query.planning.fastPath.express"};
};
extern FastPathQueryCounters fastPathQueryCounters;

class LookupPushdownCounters {
public:
    LookupPushdownCounters() = default;
    LookupPushdownCounters(LookupPushdownCounters&) = delete;
    LookupPushdownCounters& operator=(const LookupPushdownCounters&) = delete;

    void incrementLookupCountersPerQuery(int nestedLoopJoin, int indexedLoopJoin, int hashLookup) {
        nestedLoopJoinCounter.incrementRelaxed(nestedLoopJoin);
        indexedLoopJoinCounter.incrementRelaxed(indexedLoopJoin);
        hashLookupCounter.incrementRelaxed(hashLookup);
    }

    void incrementLookupCountersPerSpilling(int64_t spillToDisk, int64_t spillToDiskBytes) {
        hashLookupSpillToDisk.incrementRelaxed(spillToDisk);
        hashLookupSpillToDiskBytes.incrementRelaxed(spillToDiskBytes);
    }

    // Counters for lookup join strategies.
    Counter64& nestedLoopJoinCounter = *MetricBuilder<Counter64>{"query.lookup.nestedLoopJoin"};
    Counter64& indexedLoopJoinCounter = *MetricBuilder<Counter64>{"query.lookup.indexedLoopJoin"};
    Counter64& hashLookupCounter = *MetricBuilder<Counter64>{"query.lookup.hashLookup"};
    // Counter tracking hashLookup spills in lookup stages that get pushed down.
    Counter64& hashLookupSpillToDisk =
        *MetricBuilder<Counter64>{"query.lookup.hashLookupSpillToDisk"};
    // Counter tracking hashLookup spilled bytes in lookup stages that get pushed down.
    Counter64& hashLookupSpillToDiskBytes =
        *MetricBuilder<Counter64>{"query.lookup.hashLookupSpillToDiskBytes"};
};
extern LookupPushdownCounters lookupPushdownCounters;

class SortCounters {
public:
    void incrementSortCountersPerQuery(int64_t bytesSorted, int64_t keysSorted) {
        sortTotalBytesCounter.incrementRelaxed(bytesSorted);
        sortTotalKeysCounter.incrementRelaxed(keysSorted);
    }

    void incrementSortCountersPerSpilling(int64_t sortSpills, int64_t sortSpillBytes) {
        sortSpillsCounter.incrementRelaxed(sortSpills);
        sortSpillBytesCounter.incrementRelaxed(sortSpillBytes);
    }

    // Counters tracking sort stats across all engines
    // The total number of spills from sort stages
    Counter64& sortSpillsCounter = *MetricBuilder<Counter64>{"query.sort.spillToDisk"};
    // The total bytes spilled. This is the storage size after compression.
    Counter64& sortSpillBytesCounter = *MetricBuilder<Counter64>{"query.sort.spillToDiskBytes"};
    // The number of keys that we've sorted.
    Counter64& sortTotalKeysCounter = *MetricBuilder<Counter64>{"query.sort.totalKeysSorted"};
    // The amount of data we've sorted in bytes
    Counter64& sortTotalBytesCounter = *MetricBuilder<Counter64>{"query.sort.totalBytesSorted"};
};
extern SortCounters sortCounters;

/** Counters tracking group stats across all execution engines. */
class GroupCounters {
public:
    GroupCounters() = default;
    GroupCounters(GroupCounters&) = delete;
    GroupCounters& operator=(const GroupCounters&) = delete;

    void incrementGroupCountersPerSpilling(int64_t spills,
                                           int64_t spilledBytes,
                                           int64_t spilledRecords) {
        groupSpills.incrementRelaxed(spills);
        groupSpilledBytes.incrementRelaxed(spilledBytes);
        groupSpilledRecords.incrementRelaxed(spilledRecords);
    }

    void incrementGroupCountersPerQuery(int64_t spilledDataStorageSize) {
        groupSpilledDataStorageSize.incrementRelaxed(spilledDataStorageSize);
    }

    // The total number of spills from group stages.
    Counter64& groupSpills = *MetricBuilder<Counter64>{"query.group.spills"};
    // The total number of bytes spilled from group stages. The spilled stroage size after
    // compression might be different from the bytes spilled.
    Counter64& groupSpilledBytes = *MetricBuilder<Counter64>{"query.group.spilledBytes"};
    // The number of records spilled.
    Counter64& groupSpilledRecords = *MetricBuilder<Counter64>{"query.group.spilledRecords"};
    // The size of the file or RecordStore spilled to disk, updated after all spilling happened.
    Counter64& groupSpilledDataStorageSize =
        *MetricBuilder<Counter64>{"query.group.spilledDataStorageSize"};
};
extern GroupCounters groupCounters;

/** Counters tracking setWindowFields stats across all execution engines. */
class SetWindowFieldsCounters {
public:
    SetWindowFieldsCounters() = default;
    SetWindowFieldsCounters(SetWindowFieldsCounters&) = delete;
    SetWindowFieldsCounters& operator=(const SetWindowFieldsCounters&) = delete;

    void incrementSetWindowFieldsCountersPerSpilling(int64_t spills,
                                                     int64_t spilledBytes,
                                                     int64_t spilledRecords) {
        setWindowFieldsSpills.incrementRelaxed(spills);
        setWindowFieldsSpilledBytes.incrementRelaxed(spilledBytes);
        setWindowFieldsSpilledRecords.incrementRelaxed(spilledRecords);
    }

    // Counter tracking setWindowFields spills.
    Counter64& setWindowFieldsSpills = *MetricBuilder<Counter64>{"query.setWindowFields.spills"};
    // Counter tracking setWindowFields spilled bytes. The spilled storage size after compression
    // might be different from the bytes spilled.
    Counter64& setWindowFieldsSpilledBytes =
        *MetricBuilder<Counter64>{"query.setWindowFields.spilledBytes"};
    // Counter tracking setWindowFields spilled record number.
    Counter64& setWindowFieldsSpilledRecords =
        *MetricBuilder<Counter64>{"query.setWindowFields.spilledRecords"};
};
extern SetWindowFieldsCounters setWindowFieldsCounters;

/**
 * A common class which holds various counters related to Classic and SBE plan caches.
 */
class PlanCacheCounters {
public:
    PlanCacheCounters() = default;
    PlanCacheCounters(PlanCacheCounters&) = delete;
    PlanCacheCounters& operator=(const PlanCacheCounters&) = delete;

    void incrementClassicHitsCounter() {
        classicHits.incrementRelaxed();
    }

    void incrementClassicMissesCounter() {
        classicMisses.incrementRelaxed();
    }

    void incrementClassicSkippedCounter() {
        classicSkipped.incrementRelaxed();
    }

    void incrementClassicReplannedCounter() {
        classicReplanned.incrementRelaxed();
    }

    void incrementSbeHitsCounter() {
        sbeHits.incrementRelaxed();
    }

    void incrementSbeMissesCounter() {
        sbeMisses.incrementRelaxed();
    }

    void incrementSbeSkippedCounter() {
        sbeSkipped.incrementRelaxed();
    }

    void incrementSbeReplannedCounter() {
        sbeReplanned.incrementRelaxed();
    }

private:
    static Counter64& _makeMetric(std::string name) {
        return *MetricBuilder<Counter64>("query.planCache." + std::move(name));
    }

    // Counters that track the number of times a query plan is:
    // a) found in the cache (hits),
    // b) not found in cache (misses), or
    // c) not considered for caching hence we don't even look for it in the cache (skipped).
    // d) failed to finish trial run within budget, so we decided to replan it (replanned).
    // Split into classic and SBE, depending on which execution engine is used.
    Counter64& classicHits = _makeMetric("classic.hits");
    Counter64& classicMisses = _makeMetric("classic.misses");
    Counter64& classicSkipped = _makeMetric("classic.skipped");
    Counter64& classicReplanned = _makeMetric("classic.replanned");
    Counter64& sbeHits = _makeMetric("sbe.hits");
    Counter64& sbeMisses = _makeMetric("sbe.misses");
    Counter64& sbeSkipped = _makeMetric("sbe.skipped");
    Counter64& sbeReplanned = _makeMetric("sbe.replanned");
};
extern PlanCacheCounters planCacheCounters;

/**
 * Generic class for counters of expressions inside various MQL statements.
 */
class OperatorCounters {
public:
    explicit OperatorCounters(std::string prefix) : _prefix{std::move(prefix)} {}

    void addCounter(const std::string& name) {
        _counters[name] = &*MetricBuilder<Counter64>(_prefix + name);
    }

    void mergeCounters(const StringMap<uint64_t>& toMerge) {
        for (auto&& [name, cnt] : toMerge) {
            if (auto it = _counters.find(name); it != _counters.end()) {
                it->second->incrementRelaxed(cnt);
            }
        }
    }

private:
    const std::string _prefix;
    // Map of expressions to the number of occurrences in queries.
    StringMap<Counter64*> _counters;
};

class ValidatorCounters {
public:
    ValidatorCounters() {
        _validatorCounterMap["create"] = std::make_unique<ValidatorCounter>("create");
        _validatorCounterMap["collMod"] = std::make_unique<ValidatorCounter>("collMod");
    }

    void incrementCounters(const StringData cmdName,
                           const BSONObj& validator,
                           bool parsingSucceeded) {
        if (!validator.isEmpty()) {
            auto validatorCounter = _validatorCounterMap.find(cmdName);
            tassert(7139200,
                    str::stream() << "The validator counters are not support for the command: "
                                  << cmdName,
                    validatorCounter != _validatorCounterMap.end());
            validatorCounter->second->totalCounter.incrementRelaxed();

            if (!parsingSucceeded) {
                validatorCounter->second->failedCounter.incrementRelaxed();
            }
            if (validator.hasField("$jsonSchema")) {
                validatorCounter->second->jsonSchemaCounter.incrementRelaxed();
            }
        }
    }

private:
    struct ValidatorCounter {
        explicit ValidatorCounter(StringData name)
            : totalCounter{makeMetric(name, "total")},
              failedCounter{makeMetric(name, "failed")},
              jsonSchemaCounter{makeMetric(name, "jsonSchema")} {}

        ValidatorCounter& operator=(const ValidatorCounter&) = delete;
        ValidatorCounter(const ValidatorCounter&) = delete;

        static Counter64& makeMetric(StringData name, StringData leaf) {
            return *MetricBuilder<Counter64>{std::string{"commands."} + name + ".validator." +
                                             leaf};
        }

        Counter64& totalCounter;
        Counter64& failedCounter;
        Counter64& jsonSchemaCounter;
    };

    StringMap<std::unique_ptr<ValidatorCounter>> _validatorCounterMap;
};

extern ValidatorCounters validatorCounters;

// Global counters for expressions inside aggregation pipelines.
extern OperatorCounters operatorCountersAggExpressions;
// Global counters for match expressions.
extern OperatorCounters operatorCountersMatchExpressions;
// Global counters for accumulator expressions apply to $group.
extern OperatorCounters operatorCountersGroupAccumulatorExpressions;
// Global counters for accumulator expressions apply to $setWindowFields.
extern OperatorCounters operatorCountersWindowAccumulatorExpressions;

struct QueryCounters {
private:
    static Counter64& _makeCounter(StringData name, ClusterRole role) {
        return *MetricBuilder<Counter64>{format(FMT_STRING("query.{}"), name)}.setRole(role);
    }

    ClusterRole _role;

public:
    explicit QueryCounters(ClusterRole role) : _role{role} {}

// Preprocessor ensures that metric name == member name.
#define DCL_(name) Counter64& name{_makeCounter(#name, _role)};
    // {multi:true} updates
    DCL_(updateManyCount);
    // deleteMany calls
    DCL_(deleteManyCount);

    // targeted updateOne commands (on sharded)
    DCL_(updateOneTargetedShardedCount);
    // targeted deleteOne commands (on sharded)
    DCL_(deleteOneTargetedShardedCount);
    // targeted findAndModify commands (on sharded)
    DCL_(findAndModifyTargetedShardedCount);

    // updateOne commands (on unsharded)
    DCL_(updateOneUnshardedCount);
    // deleteOne commands (on unsharded)
    DCL_(deleteOneUnshardedCount);
    // findAndModify commands (on unsharded)
    DCL_(findAndModifyUnshardedCount);

    // non-targeted updateOne commands on sharded collections
    DCL_(updateOneNonTargetedShardedCount);
    // non-targeted deleteOne commands on sharded collections
    DCL_(deleteOneNonTargetedShardedCount);
    // non-targeted findAndModify commands on sharded collections
    DCL_(findAndModifyNonTargetedShardedCount);

    // non-targeted retryable deleteOne commands
    // without shard key, with _id
    DCL_(deleteOneWithoutShardKeyWithIdCount);
    // non-targeted non-retryable deleteOne commands
    // without shard key, with _id
    DCL_(nonRetryableDeleteOneWithoutShardKeyWithIdCount);
    // non-targeted retryable updateOne commands
    // without shard key, with _id
    DCL_(updateOneWithoutShardKeyWithIdCount);
    // non-targeted non-retryable updateOne commands
    // without shard key, with _id
    DCL_(nonRetryableUpdateOneWithoutShardKeyWithIdCount);
    // retries of non-targeted updateOne commands
    // without shard key, with _id
    DCL_(updateOneWithoutShardKeyWithIdRetryCount);
    // retries of non-targeted deleteOne commands
    // without shard key, with _id
    DCL_(deleteOneWithoutShardKeyWithIdRetryCount);

    // internal retryable writes
    DCL_(internalRetryableWriteCount);
    // external retryable writes
    DCL_(externalRetryableWriteCount);
    // internal transactions for retryable writes
    DCL_(retryableInternalTransactionCount);

    // {multi:false} updates with an exact match on _id that
    // are broadcasted to multiple shards.
    DCL_(updateOneOpStyleBroadcastWithExactIDCount)
#undef DCL_
};

/** Returns the appropriate QueryCounters instance for `opCtx`'s service. */
QueryCounters& getQueryCounters(OperationContext* opCtx);

}  // namespace mongo
