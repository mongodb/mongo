// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection_options_gen.h"
#include "mongo/db/stats/opcounters.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/platform/atomic.h"
#include "mongo/rpc/message.h"
#include "mongo/util/aligned.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class NetworkCounter {
public:
    enum class ConnectionType { kIngress = 1, kEgress = 2 };

    NetworkCounter();

    NetworkCounter(const NetworkCounter&) = delete;
    NetworkCounter& operator=(const NetworkCounter&) = delete;

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
    // Physical byte counters — not OTel-exported.
    // TODO SERVER-127423: Replace these with OTel-exported counters.
    CacheExclusive<Atomic<long long>> _ingressPhysicalBytesIn{0};
    CacheExclusive<Atomic<long long>> _ingressPhysicalBytesOut{0};
    CacheExclusive<Atomic<long long>> _egressPhysicalBytesIn{0};
    CacheExclusive<Atomic<long long>> _egressPhysicalBytesOut{0};

    // Logical ingress counters.
    otel::metrics::Counter<int64_t>& _ingressLogicalBytesIn;
    otel::metrics::Counter<int64_t>& _ingressNumRequests;
    otel::metrics::Counter<int64_t>& _ingressLogicalBytesOut;

    // Logical egress counters.
    otel::metrics::Counter<int64_t>& _egressLogicalBytesIn;
    otel::metrics::Counter<int64_t>& _egressNumRequests;
    otel::metrics::Counter<int64_t>& _egressLogicalBytesOut;

    otel::metrics::Counter<int64_t>& _numSlowDNSOperations;
    otel::metrics::Counter<int64_t>& _numSlowSSLOperations;

    // Counter of inbound connections at runtime.
    // TODO SERVER-127423: Replace this with an OTel-exported counter.
    CacheExclusive<Atomic<std::int64_t>> _tfoAccepted{0};

    // TFO info determined at startup.
    std::int64_t _tfoKernelSetting{0};
    bool _tfoKernelSupportServer{false};
    bool _tfoKernelSupportClient{false};
};

/** Returns the process-global NetworkCounter. */
NetworkCounter& globalNetworkCounter();

class AuthCounter {
    struct MechanismData;

public:
    class IngressMechanismCounterHandle {
    public:
        IngressMechanismCounterHandle(MechanismData* data) : _data(data) {
            invariant(data->ingressAllowed);
        }

        void incSpeculativeAuthenticateReceived();
        void incIngressSpeculativeAuthenticateSuccessful();

        void incAuthenticateReceived();
        void incIngressAuthenticateSuccessful();

        void incClusterAuthenticateReceived();
        void incClusterAuthenticateSuccessful();

    private:
        MechanismData* _data;
    };

    class EgressMechanismCounterHandle {
    public:
        EgressMechanismCounterHandle(MechanismData* data) : _data(data) {}

        void incSpeculativeAuthenticateSent();
        void incEgressSpeculativeAuthenticateSuccessful();

        void incAuthenticateSent();
        void incEgressAuthenticateSuccessful();


    private:
        MechanismData* _data;
    };

    IngressMechanismCounterHandle getIngressMechanismCounter(std::string_view mechanism);
    EgressMechanismCounterHandle getEgressMechanismCounter(std::string_view mechanism);

    void incSaslSupportedMechanismsReceived();

    void incIngressAuthenticationCumulativeTime(long long micros);

    void incEgressAuthenticationCumulativeTime(long long micros);

    void append(BSONObjBuilder*);

    void initializeMechanismMap(const std::vector<std::string>&);

private:
    struct SuccessCounter {
        Atomic<long long> total;
        Atomic<long long> successful;
        void appendAsSubobj(BSONObjBuilder& bob, std::string_view fieldName) const;
    };
    struct MechanismData {
        struct {
            SuccessCounter speculativeAuthenticate;
            SuccessCounter authenticate;
            SuccessCounter clusterAuthenticate;
        } ingress;
        struct {
            SuccessCounter speculativeAuthenticate;
            SuccessCounter authenticate;
        } egress;
        bool ingressAllowed = false;
    };
    using MechanismMap = std::map<std::string, MechanismData, std::less<>>;

    Atomic<long long> _saslSupportedMechanismsReceived;
    Atomic<long long> _ingressAuthenticationCumulativeMicros;
    Atomic<long long> _egressAuthenticationCumulativeMicros;
    // Mechanism maps are initialized at startup to contain all possible mechanisms. Mechanisms not
    // present in the authenticationMechanisms setParam are marked as egress only; this is
    // because this parameter only restricts which mechanisms can be used for ingress.  After
    // startup, the set of enabled ingress mechanisms is fixed.
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
    void increment(std::string_view name, long long n = 1) {
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
    QueryFrameworkCounters();
    QueryFrameworkCounters(QueryFrameworkCounters&) = delete;
    QueryFrameworkCounters& operator=(const QueryFrameworkCounters&) = delete;

    void incrementQueryEngineCounters(CurOp* curop) {
        auto& debug = curop->debug();
        const BSONObj& cmdObj = curop->opDescription();
        auto cmdName = cmdObj.firstElementFieldNameStringData();

        if (cmdName == "find") {
            switch (debug.queryFramework) {
                case PlanExecutor::QueryFramework::kClassicOnly:
                    incrementFindClassicCounter();
                    break;
                case PlanExecutor::QueryFramework::kSBEOnly:
                    incrementFindSbeCounter();
                    break;
                default:
                    break;
            }
        } else if (cmdName == "aggregate") {
            switch (debug.queryFramework) {
                case PlanExecutor::QueryFramework::kClassicOnly:
                    incrementAggregateClassicOnlyCounter();
                    break;
                case PlanExecutor::QueryFramework::kClassicHybrid:
                    incrementAggregateClassicHybridCounter();
                    break;
                case PlanExecutor::QueryFramework::kSBEOnly:
                    incrementAggregateSbeOnlyCounter();
                    break;
                case PlanExecutor::QueryFramework::kSBEHybrid:
                    incrementAggregateSbeHybridCounter();
                    break;
                case PlanExecutor::QueryFramework::kUnknown:
                    break;
            }
        }
    }

    void incrementFindSbeCounter() {
        sbeFindQueryCounter.add(1);
    }
    void incrementFindClassicCounter() {
        classicFindQueryCounter.add(1);
    }
    void incrementAggregateSbeOnlyCounter() {
        sbeOnlyAggregationCounter.add(1);
    }
    void incrementAggregateClassicOnlyCounter() {
        classicOnlyAggregationCounter.add(1);
    }
    void incrementAggregateSbeHybridCounter() {
        sbeHybridAggregationCounter.add(1);
    }
    void incrementAggregateClassicHybridCounter() {
        classicHybridAggregationCounter.add(1);
    }

private:
    // Query counters that record whether a find query was fully or partially executed in SBE, or
    // fully executed using the classic engine. One of these will always be incremented during a
    // query.
    otel::metrics::Counter<int64_t>& sbeFindQueryCounter;
    otel::metrics::Counter<int64_t>& classicFindQueryCounter;

    // Aggregation query counters that record whether an aggregation was fully or partially executed
    // in DocumentSource (an sbe/classic hybrid plan), or fully pushed down to the sbe/classic
    // layer. These are only incremented during aggregations.
    otel::metrics::Counter<int64_t>& sbeOnlyAggregationCounter;
    otel::metrics::Counter<int64_t>& classicOnlyAggregationCounter;
    otel::metrics::Counter<int64_t>& sbeHybridAggregationCounter;
    otel::metrics::Counter<int64_t>& classicHybridAggregationCounter;
};
extern QueryFrameworkCounters queryFrameworkCounters;

class FastPathQueryCounters {
public:
    FastPathQueryCounters();
    FastPathQueryCounters(FastPathQueryCounters&) = delete;
    FastPathQueryCounters& operator=(const FastPathQueryCounters&) = delete;

    void incrementIdHackQueryCounter() {
        idHackQueryCounter.add(1);
    }

    void incrementExpressQueryCounter() {
        expressQueryCounter.add(1);
    }

private:
    // Counter for the number of queries planned using idHack fast planning.
    otel::metrics::Counter<int64_t>& idHackQueryCounter;
    // Counter for the number of queries planned using express fast planning.
    otel::metrics::Counter<int64_t>& expressQueryCounter;
};
extern FastPathQueryCounters fastPathQueryCounters;

class SpillingCounters {
public:
    enum SuffixStyle { kDotSuffix, kUpperCaseSuffix };

    SpillingCounters(std::string stageName, SuffixStyle suffixStyle = kDotSuffix)
        : spills(
              *MetricBuilder<Counter64>{"query." + stageName + _getSuffix(suffixStyle, "spills")}),
          spilledBytes(*MetricBuilder<Counter64>{"query." + stageName +
                                                 _getSuffix(suffixStyle, "spilledBytes")}),
          spilledRecords(*MetricBuilder<Counter64>{"query." + stageName +
                                                   _getSuffix(suffixStyle, "spilledRecords")}),
          spilledDataStorageSize(*MetricBuilder<Counter64>{
              "query." + stageName + _getSuffix(suffixStyle, "spilledDataStorageSize")}) {}

    SpillingCounters(SpillingCounters&) = delete;
    SpillingCounters& operator=(const SpillingCounters&) = delete;

    virtual ~SpillingCounters() = default;

    void incrementPerSpilling(int64_t spills,
                              int64_t spilledBytes,
                              int64_t spilledRecords,
                              int64_t spilledDataStorageSize) {
        this->spills.incrementRelaxed(spills);
        this->spilledBytes.incrementRelaxed(spilledBytes);
        this->spilledRecords.incrementRelaxed(spilledRecords);
        this->spilledDataStorageSize.incrementRelaxed(spilledDataStorageSize);
    }

    // The total number of spills.
    Counter64& spills;
    // The total number of bytes spilled. The spilled storage size after compression might be
    // different from the bytes spilled.
    Counter64& spilledBytes;
    // The number of records spilled.
    Counter64& spilledRecords;
    // The size of the file or RecordStore spilled to disk, updated after all spilling happened.
    Counter64& spilledDataStorageSize;

private:
    std::string _getSuffix(SuffixStyle suffixStyle, std::string metricName) {
        switch (suffixStyle) {
            case kDotSuffix:
                return "." + metricName;
            case kUpperCaseSuffix:
                return "S" + metricName.substr(1);
        }
        MONGO_UNREACHABLE_TASSERT(10916900);
    }
};

class LookupPushdownCounters : public SpillingCounters {
public:
    LookupPushdownCounters()
        : SpillingCounters("lookup.hashLookup", SpillingCounters::kUpperCaseSuffix) {}
    LookupPushdownCounters(LookupPushdownCounters&) = delete;
    LookupPushdownCounters& operator=(const LookupPushdownCounters&) = delete;

    void incrementLookupCountersPerQuery(int nestedLoopJoin,
                                         int indexedLoopJoin,
                                         int hashLookup,
                                         int dynamicIndexedLoopJoin) {
        nestedLoopJoinCounter.incrementRelaxed(nestedLoopJoin);
        indexedLoopJoinCounter.incrementRelaxed(indexedLoopJoin);
        hashLookupCounter.incrementRelaxed(hashLookup);
        dynamicIndexedLoopJoinCounter.incrementRelaxed(dynamicIndexedLoopJoin);
    }

    void incrementPerSpilling(int64_t spills,
                              int64_t spilledBytes,
                              int64_t spilledRecords,
                              int64_t spilledDataStorageSize) {
        SpillingCounters::incrementPerSpilling(
            spills, spilledBytes, spilledRecords, spilledDataStorageSize);

        //  Counters for backward compatiblity.
        hashLookupSpillToDisk.incrementRelaxed(spills);
        hashLookupSpillToDiskBytes.incrementRelaxed(spilledBytes);
    }

    // Counters for lookup join strategies.
    Counter64& nestedLoopJoinCounter = *MetricBuilder<Counter64>{"query.lookup.nestedLoopJoin"};
    Counter64& indexedLoopJoinCounter = *MetricBuilder<Counter64>{"query.lookup.indexedLoopJoin"};
    Counter64& hashLookupCounter = *MetricBuilder<Counter64>{"query.lookup.hashLookup"};
    Counter64& dynamicIndexedLoopJoinCounter =
        *MetricBuilder<Counter64>{"query.lookup.dynamicIndexedLoopJoin"};

    // Duplicate spilling counters, not deleted to maintain backward compatibility.
    // Counter tracking hashLookup spills in lookup stages that get pushed down.
    Counter64& hashLookupSpillToDisk =
        *MetricBuilder<Counter64>{"query.lookup.hashLookupSpillToDisk"};
    // Counter tracking hashLookup spilled bytes in lookup stages that get pushed down.
    Counter64& hashLookupSpillToDiskBytes =
        *MetricBuilder<Counter64>{"query.lookup.hashLookupSpillToDiskBytes"};
};
extern LookupPushdownCounters lookupPushdownCounters;

/**
 * Counters tracking $lookup+$unwind (LU) IFR flag metrics: join strategy and local-side plan shape.
 * TODO SERVER-128934: Add spill counters once LU and plain $lookup spill paths are separated.
 */
class LookupUnwindPushdownCounters {
public:
    LookupUnwindPushdownCounters() = default;
    LookupUnwindPushdownCounters(LookupUnwindPushdownCounters&) = delete;
    LookupUnwindPushdownCounters& operator=(const LookupUnwindPushdownCounters&) = delete;

    void incrementLookupUnwindCountersPerQuery(int luIndexedLoopJoin,
                                               int luNestedLoopJoin,
                                               int luHashLookup,
                                               int luDynamicIndexedLoopJoin,
                                               int luLocalCollscan,
                                               int luLocalIxscanFetch,
                                               int luLocalComplex) {
        inljCounter.incrementRelaxed(luIndexedLoopJoin);
        nljCounter.incrementRelaxed(luNestedLoopJoin);
        hjCounter.incrementRelaxed(luHashLookup);
        dinljCounter.incrementRelaxed(luDynamicIndexedLoopJoin);
        localCollscanCounter.incrementRelaxed(luLocalCollscan);
        localIxscanFetchCounter.incrementRelaxed(luLocalIxscanFetch);
        localComplexCounter.incrementRelaxed(luLocalComplex);
    }

    Counter64& inljCounter = *MetricBuilder<Counter64>{"query.lookupUnwind.indexedLoopJoin"};
    Counter64& nljCounter = *MetricBuilder<Counter64>{"query.lookupUnwind.nestedLoopJoin"};
    Counter64& hjCounter = *MetricBuilder<Counter64>{"query.lookupUnwind.hashLookup"};
    Counter64& dinljCounter =
        *MetricBuilder<Counter64>{"query.lookupUnwind.dynamicIndexedLoopJoin"};
    Counter64& localCollscanCounter = *MetricBuilder<Counter64>{"query.lookupUnwind.localCollscan"};
    Counter64& localIxscanFetchCounter =
        *MetricBuilder<Counter64>{"query.lookupUnwind.localIxscanFetch"};
    Counter64& localComplexCounter = *MetricBuilder<Counter64>{"query.lookupUnwind.localComplex"};
};
extern LookupUnwindPushdownCounters lookupUnwindPushdownCounters;

/**
 * Counters tracking non-leading pushdown operators.
 */
class NonLeadingPushdownCounters {
public:
    NonLeadingPushdownCounters() = default;
    NonLeadingPushdownCounters(const NonLeadingPushdownCounters&) = delete;
    NonLeadingPushdownCounters& operator=(const NonLeadingPushdownCounters&) = delete;

    void incrementCounters(bool nlpMatch, bool nlpProject, bool nlpAddFields, bool nlpReplaceRoot) {
        if (nlpMatch)
            matchCounter.incrementRelaxed(1);
        if (nlpProject)
            projectCounter.incrementRelaxed(1);
        if (nlpAddFields)
            addFieldsCounter.incrementRelaxed(1);
        if (nlpReplaceRoot)
            replaceRootCounter.incrementRelaxed(1);
    }

    Counter64& matchCounter = *MetricBuilder<Counter64>{"query.nonLeadingPushdown.match"};
    Counter64& projectCounter = *MetricBuilder<Counter64>{"query.nonLeadingPushdown.project"};
    Counter64& addFieldsCounter = *MetricBuilder<Counter64>{"query.nonLeadingPushdown.addFields"};
    Counter64& replaceRootCounter =
        *MetricBuilder<Counter64>{"query.nonLeadingPushdown.replaceRoot"};
};
extern NonLeadingPushdownCounters nonLeadingPushdownCounters;

/** Counters tracking group stats across all execution engines. */
class GroupCounters : public SpillingCounters {
public:
    GroupCounters() : SpillingCounters("group") {}
    GroupCounters(GroupCounters&) = delete;
};
extern GroupCounters groupCounters;

/** Counters tracking setWindowFields stats across all execution engines. */
class SetWindowFieldsCounters : public SpillingCounters {
public:
    SetWindowFieldsCounters() : SpillingCounters("setWindowFields") {}
};
extern SetWindowFieldsCounters setWindowFieldsCounters;

/** Counters tracking graphLookup stats. */
class GraphLookupCounters : public SpillingCounters {
public:
    GraphLookupCounters() : SpillingCounters("graphLookup") {}
};
extern GraphLookupCounters graphLookupCounters;

class TextOrCounters : public SpillingCounters {
public:
    TextOrCounters() : SpillingCounters("textOr") {}
};
extern TextOrCounters textOrCounters;

class BucketAutoCounters : public SpillingCounters {
public:
    BucketAutoCounters() : SpillingCounters("bucketAuto") {}
};
extern BucketAutoCounters bucketAutoCounters;

class GeoNearCounters : public SpillingCounters {
public:
    GeoNearCounters() : SpillingCounters("geoNear") {}
};
extern GeoNearCounters geoNearCounters;

/** Counters tracking HashJoin stats */
class HashJoinCounters : public SpillingCounters {
public:
    HashJoinCounters() : SpillingCounters("hashJoin") {}
};
extern HashJoinCounters hashJoinCounters;

class RecordIdDeduplicationCounters {
public:
    RecordIdDeduplicationCounters(std::string stageName)
        : deduplicatedBytes(*MetricBuilder<Counter64>{"query.recordIdDeduplication." + stageName +
                                                      ".deduplicatedBytes"}),
          deduplicatedRecords(*MetricBuilder<Counter64>{"query.recordIdDeduplication." + stageName +
                                                        ".deduplicatedRecords"}) {}

    RecordIdDeduplicationCounters(RecordIdDeduplicationCounters&) = delete;
    RecordIdDeduplicationCounters& operator=(const RecordIdDeduplicationCounters&) = delete;

    virtual ~RecordIdDeduplicationCounters() = default;

    void incrementPerDeduplication(int64_t deduplicatedBytes, int64_t deduplicatedRecords) {
        this->deduplicatedBytes.incrementRelaxed(deduplicatedBytes);
        this->deduplicatedRecords.incrementRelaxed(deduplicatedRecords);
    }

private:
    // The total number of bytes deduplicated.
    Counter64& deduplicatedBytes;
    // The number of records deduplicated.
    Counter64& deduplicatedRecords;
};

class OrCounters : public RecordIdDeduplicationCounters {
public:
    OrCounters() : RecordIdDeduplicationCounters("OR") {}
};
extern OrCounters orCounters;

class SortMergeCounters : public RecordIdDeduplicationCounters {
public:
    SortMergeCounters() : RecordIdDeduplicationCounters("SORT_MERGE") {}
};
extern SortMergeCounters sortMergeCounters;

class IxScanCounters : public RecordIdDeduplicationCounters {
public:
    IxScanCounters() : RecordIdDeduplicationCounters("IXSCAN") {}
};
extern IxScanCounters ixScanCounters;

class UniqueCounters : public RecordIdDeduplicationCounters {
public:
    UniqueCounters() : RecordIdDeduplicationCounters("unique") {}
};
extern UniqueCounters uniqueCounters;

class UniqueRoaringCounters : public RecordIdDeduplicationCounters {
public:
    UniqueRoaringCounters() : RecordIdDeduplicationCounters("unique_roaring") {}
};
extern UniqueRoaringCounters uniqueRoaringCounters;

class CountScanCounters : public RecordIdDeduplicationCounters {
public:
    CountScanCounters() : RecordIdDeduplicationCounters("COUNT_SCAN") {}
};
extern CountScanCounters countScanCounters;

class NearCounters : public RecordIdDeduplicationCounters {
public:
    NearCounters() : RecordIdDeduplicationCounters("NEAR") {}
};
extern NearCounters nearCounters;

class UpdateCounters : public RecordIdDeduplicationCounters {
public:
    UpdateCounters() : RecordIdDeduplicationCounters("UPDATE") {}
};
extern UpdateCounters updateCounters;

/**
 * A common class which holds various counters related to Classic and SBE plan caches.
 */
class PlanCacheCounters {
public:
    PlanCacheCounters();
    PlanCacheCounters(PlanCacheCounters&) = delete;
    PlanCacheCounters& operator=(const PlanCacheCounters&) = delete;

    void incrementClassicHitsCounter() {
        classicHits.add(1);
    }

    void incrementClassicMissesCounter() {
        classicMisses.add(1);
    }

    void incrementClassicSkippedCounter() {
        classicSkipped.add(1);
    }

    void incrementClassicReplannedCounter() {
        classicReplanned.add(1);
    }

    void incrementClassicReplannedPlanIsCachedPlanCounter() {
        classicReplannedPlanIsCachedPlan.add(1);
    }

    void incrementClassicCachedPlansEvictedCounter(size_t increment) {
        classicCachedPlansEvicted.add(static_cast<int64_t>(increment));
    }

    void incrementClassicInactiveCachedPlansReplacedCounter() {
        classicInactiveCachedPlansReplaced.add(1);
    }

    void incrementSbeHitsCounter() {
        sbeHits.add(1);
    }

    void incrementSbeMissesCounter() {
        sbeMisses.add(1);
    }

    void incrementSbeSkippedCounter() {
        sbeSkipped.add(1);
    }

    void incrementSbeReplannedCounter() {
        sbeReplanned.add(1);
    }

    void incrementSbeReplannedPlanIsCachedPlanCounter() {
        sbeReplannedPlanIsCachedPlan.add(1);
    }

    void incrementSbeCachedPlansEvictedCounter(size_t increment) {
        sbeCachedPlansEvicted.add(static_cast<int64_t>(increment));
    }

    void incrementSbeInactiveCachedPlansReplacedCounter() {
        sbeInactiveCachedPlansReplaced.add(1);
    }

private:
    // Counters that track the number of times a query plan is:
    // a) found in the cache (hits),
    // b) not found in cache (misses), or
    // c) not considered for caching hence we don't even look for it in the cache (skipped);
    // d) failed to finish trial run within budget, so we decided to replan it (replanned);
    // e) replanned only to produce the same plan as what's in the plan cache.
    // Split into classic and SBE, depending on which execution engine is used.
    otel::metrics::Counter<int64_t>& classicHits;
    otel::metrics::Counter<int64_t>& classicMisses;
    otel::metrics::Counter<int64_t>& classicSkipped;
    otel::metrics::Counter<int64_t>& classicReplanned;
    otel::metrics::Counter<int64_t>& classicReplannedPlanIsCachedPlan;
    otel::metrics::Counter<int64_t>& classicCachedPlansEvicted;
    otel::metrics::Counter<int64_t>& classicInactiveCachedPlansReplaced;
    otel::metrics::Counter<int64_t>& sbeHits;
    otel::metrics::Counter<int64_t>& sbeMisses;
    otel::metrics::Counter<int64_t>& sbeSkipped;
    otel::metrics::Counter<int64_t>& sbeReplanned;
    otel::metrics::Counter<int64_t>& sbeReplannedPlanIsCachedPlan;
    otel::metrics::Counter<int64_t>& sbeCachedPlansEvicted;
    otel::metrics::Counter<int64_t>& sbeInactiveCachedPlansReplaced;
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

class TimeseriesCounters {
public:
    TimeseriesCounters() = default;
    TimeseriesCounters(TimeseriesCounters&) = delete;
    TimeseriesCounters& operator=(const TimeseriesCounters&) = delete;

    void incrementDirectDeleted() {
        directDeleted.incrementRelaxed();
    }

    void incrementDirectUpdated() {
        directUpdated.incrementRelaxed();
    }

    void incrementMeasurementDelete() {
        measurementDelete.incrementRelaxed();
    }

    void incrementMeasurementUpdate() {
        measurementUpdate.incrementRelaxed();
    }

    void incrementMetaDelete() {
        metaDelete.incrementRelaxed();
    }

    void incrementMetaUpdate() {
        metaUpdate.incrementRelaxed();
    }

    // Number of direct writes to bucket documents from all kinds of external and internal
    // operations. Only counts operations that commit a write to storage.
    Counter64& directDeleted = *MetricBuilder<Counter64>{"timeseries.directDeleted"};
    Counter64& directUpdated = *MetricBuilder<Counter64>{"timeseries.directUpdated"};

    // Number of user deletes performed at measurement level.
    Counter64& measurementDelete = *MetricBuilder<Counter64>{"timeseries.measurementDelete"};

    // Number of user updates.
    Counter64& measurementUpdate = *MetricBuilder<Counter64>{"timeseries.measurementUpdate"};

    // Number of user deletes performed at bucket level.
    Counter64& metaDelete = *MetricBuilder<Counter64>{"timeseries.metaDelete"};

    // Number of user updates performed at bucket level.
    Counter64& metaUpdate = *MetricBuilder<Counter64>{"timeseries.metaUpdate"};
};
extern TimeseriesCounters timeseriesCounters;

class ValidatorCounters {
public:
    ValidatorCounters() {
        _validatorCounterMap["create"] = std::make_unique<ValidatorCounter>("create");
        _validatorCounterMap["collMod"] = std::make_unique<ValidatorCounter>("collMod");
    }

    void incrementCounters(const std::string_view cmdName,
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
        explicit ValidatorCounter(std::string_view name)
            : totalCounter{makeMetric(name, "total")},
              failedCounter{makeMetric(name, "failed")},
              jsonSchemaCounter{makeMetric(name, "jsonSchema")} {}

        ValidatorCounter& operator=(const ValidatorCounter&) = delete;
        ValidatorCounter(const ValidatorCounter&) = delete;

        static Counter64& makeMetric(std::string_view name, std::string_view leaf) {
            return *MetricBuilder<Counter64>{fmt::format("commands.{}.validator.{}", name, leaf)};
        }

        Counter64& totalCounter;
        Counter64& failedCounter;
        Counter64& jsonSchemaCounter;
    };

    StringMap<std::unique_ptr<ValidatorCounter>> _validatorCounterMap;
};

extern ValidatorCounters validatorCounters;

class ValidationLevelCounters {
public:
    ValidationLevelCounters() {
        _validationLevelCounterMap["create"] = std::make_unique<ValidationLevelCounter>("create");
        _validationLevelCounterMap["collMod"] = std::make_unique<ValidationLevelCounter>("collMod");
    }

    // Passing boost::none counts as "default" (validator present, no explicit level).
    void increment(std::string_view cmdName, boost::optional<ValidationLevelEnum> level) {
        auto it = _validationLevelCounterMap.find(cmdName);
        tassert(12371400,
                str::stream() << "Validation level counters not supported for command: " << cmdName,
                it != _validationLevelCounterMap.end());
        if (!level) {
            it->second->defaultLevel.incrementRelaxed();
            return;
        }
        switch (*level) {
            case ValidationLevelEnum::off:
                it->second->off.incrementRelaxed();
                break;
            case ValidationLevelEnum::moderate:
                it->second->moderate.incrementRelaxed();
                break;
            case ValidationLevelEnum::strict:
                it->second->strict.incrementRelaxed();
                break;
            case ValidationLevelEnum::constraint:
                it->second->constraint.incrementRelaxed();
                break;
        }
    }

private:
    struct ValidationLevelCounter {
        explicit ValidationLevelCounter(std::string_view name)
            : defaultLevel{makeMetric(name, "default")},
              off{makeMetric(name, "off")},
              moderate{makeMetric(name, "moderate")},
              strict{makeMetric(name, "strict")},
              constraint{makeMetric(name, "constraint")} {}

        ValidationLevelCounter& operator=(const ValidationLevelCounter&) = delete;
        ValidationLevelCounter(const ValidationLevelCounter&) = delete;

        static Counter64& makeMetric(std::string_view name, std::string_view level) {
            return *MetricBuilder<Counter64>{
                fmt::format("commands.{}.validationLevel.{}", name, level)};
        }

        Counter64& defaultLevel;
        Counter64& off;
        Counter64& moderate;
        Counter64& strict;
        Counter64& constraint;
    };

    StringMap<std::unique_ptr<ValidationLevelCounter>> _validationLevelCounterMap;
};

extern ValidationLevelCounters validationLevelCounters;

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
    static Counter64& _makeCounter(std::string_view name, ClusterRole role) {
        return *MetricBuilder<Counter64>{fmt::format("query.{}", name)}.setRole(role);
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

template <typename DurationType>
class DurationCounter64 {
public:
    void increment(DurationType d) {
        _counter.increment(d.count());
    }

    DurationType get() const {
        return DurationType{_counter.get()};
    }

private:
    Counter64 _counter;
};

template <typename D>
struct ServerStatusMetricPolicySelection<DurationCounter64<D>> {
    struct Policy {
        DurationCounter64<D>& value() {
            return _v;
        }

        void appendTo(BSONObjBuilder& b, std::string_view leafName) const {
            b.append(leafName, static_cast<long long>(_v.get().count()));
        }

    private:
        DurationCounter64<D> _v;
    };

    using type = Policy;
};

}  // namespace mongo
