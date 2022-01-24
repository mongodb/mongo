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

#include <map>

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/message.h"
#include "mongo/util/aligned.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/processinfo.h"
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

    void gotInsertsDeprecated(int n) {
        _checkWrap(&OpCounters::_insertDeprecated, n);
    }
    void gotQueryDeprecated() {
        _checkWrap(&OpCounters::_queryDeprecated, 1);
    }
    void gotUpdateDeprecated() {
        _checkWrap(&OpCounters::_updateDeprecated, 1);
    }
    void gotDeleteDeprecated() {
        _checkWrap(&OpCounters::_deleteDeprecated, 1);
    }
    void gotGetMoreDeprecated() {
        _checkWrap(&OpCounters::_getmoreDeprecated, 1);
    }
    void gotKillCursorsDeprecated() {
        _checkWrap(&OpCounters::_killcursorsDeprecated, 1);
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

    // thse are used by snmp, and other things, do not remove
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

private:
    // Increment member `counter` by `n`, resetting all counters if it was > 2^60.
    void _checkWrap(CacheAligned<AtomicWord<long long>> OpCounters::*counter, int n);

    CacheAligned<AtomicWord<long long>> _insert;
    CacheAligned<AtomicWord<long long>> _query;
    CacheAligned<AtomicWord<long long>> _update;
    CacheAligned<AtomicWord<long long>> _delete;
    CacheAligned<AtomicWord<long long>> _getmore;
    CacheAligned<AtomicWord<long long>> _command;

    CacheAligned<AtomicWord<long long>> _insertOnExistingDoc;
    CacheAligned<AtomicWord<long long>> _updateOnMissingDoc;
    CacheAligned<AtomicWord<long long>> _deleteWasEmpty;
    CacheAligned<AtomicWord<long long>> _deleteFromMissingNamespace;
    CacheAligned<AtomicWord<long long>> _acceptableErrorInCommand;

    // Counters for deprecated opcodes.
    CacheAligned<AtomicWord<long long>> _insertDeprecated;
    CacheAligned<AtomicWord<long long>> _queryDeprecated;
    CacheAligned<AtomicWord<long long>> _updateDeprecated;
    CacheAligned<AtomicWord<long long>> _deleteDeprecated;
    CacheAligned<AtomicWord<long long>> _getmoreDeprecated;
    CacheAligned<AtomicWord<long long>> _killcursorsDeprecated;
};

extern OpCounters globalOpCounters;
extern OpCounters replOpCounters;

class NetworkCounter {
public:
    // Increment the counters for the number of bytes read directly off the wire
    void hitPhysicalIn(long long bytes);
    void hitPhysicalOut(long long bytes);

    // Increment the counters for the number of bytes passed out of the TransportLayer to the
    // server
    void hitLogicalIn(long long bytes);
    void hitLogicalOut(long long bytes);

    // Increment the counter for the number of slow dns resolution operations.
    void incrementNumSlowDNSOperations();

    // Increment the counter for the number of slow ssl handshake operations.
    void incrementNumSlowSSLOperations();

    // TFO Counters and Status;
    void acceptedTFOIngress();

    void setTFOKernelSetting(std::int64_t val) {
        _tfo->kernelSetting = val;
    }

    void setTFOServerSupport(bool val) {
        _tfo->kernelSupportServer = val;
    }

    void setTFOClientSupport(bool val) {
        _tfo->kernelSupportClient = val;
    }

    void append(BSONObjBuilder& b);

private:
    CacheAligned<AtomicWord<long long>> _physicalBytesIn{0};
    CacheAligned<AtomicWord<long long>> _physicalBytesOut{0};

    // These two counters are always incremented at the same time, so
    // we place them on the same cache line.
    struct Together {
        AtomicWord<long long> logicalBytesIn{0};
        AtomicWord<long long> requests{0};
    };
    CacheAligned<Together> _together{};
    static_assert(sizeof(decltype(_together)) <= stdx::hardware_constructive_interference_size,
                  "cache line spill");

    CacheAligned<AtomicWord<long long>> _logicalBytesOut{0};

    CacheAligned<AtomicWord<long long>> _numSlowDNSOperations{0};
    CacheAligned<AtomicWord<long long>> _numSlowSSLOperations{0};

    struct TFO {
        // Counter of inbound connections at runtime.
        AtomicWord<std::int64_t> accepted{0};

        // Info determined at startup.
        std::int64_t kernelSetting;
        bool kernelSupportServer{false};
        bool kernelSupportClient{false};
    };
    CacheAligned<TFO> _tfo{};
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

    // Mechanism maps are initialized at startup to contain all
    // mechanisms known to authenticationMechanisms setParam.
    // After that they are kept to a fixed size.
    MechanismMap _mechanisms;
};
extern AuthCounter authCounter;

class AggStageCounters {
public:
    // Container for a stage count metric along with its corresponding counter.
    struct StageCounter {
        StageCounter(StringData name) : metric("aggStageCounters." + name, &counter) {}

        Counter64 counter;
        ServerStatusMetricField<Counter64> metric;
    };

    // Map of aggregation stages to the number of occurrences.
    StringMap<std::unique_ptr<StageCounter>> stageCounterMap = {};
};

extern AggStageCounters aggStageCounters;

class DotsAndDollarsFieldsCounters {
public:
    DotsAndDollarsFieldsCounters()
        : insertMetric("dotsAndDollarsFields.inserts", &inserts),
          updateMetric("dotsAndDollarsFields.updates", &updates) {}

    void incrementForUpsert(bool didInsert) {
        if (didInsert) {
            inserts.increment();
        } else {
            updates.increment();
        }
    }

    Counter64 inserts;
    Counter64 updates;
    ServerStatusMetricField<Counter64> insertMetric;
    ServerStatusMetricField<Counter64> updateMetric;
};

extern DotsAndDollarsFieldsCounters dotsAndDollarsFieldsCounters;

class QueryEngineCounters {
public:
    QueryEngineCounters()
        : sbeFindQueryMetric("query.queryExecutionEngine.find.sbe", &sbeFindQueryCounter),
          classicFindQueryMetric("query.queryExecutionEngine.find.classic",
                                 &classicFindQueryCounter),
          sbeOnlyAggregationMetric("query.queryExecutionEngine.aggregate.sbeOnly",
                                   &sbeOnlyAggregationCounter),
          classicOnlyAggregationMetric("query.queryExecutionEngine.aggregate.classicOnly",
                                       &classicOnlyAggregationCounter),
          sbeHybridAggregationMetric("query.queryExecutionEngine.aggregate.sbeHybrid",
                                     &sbeHybridAggregationCounter),
          classicHybridAggregationMetric("query.queryExecutionEngine.aggregate.classicHybrid",
                                         &classicHybridAggregationCounter) {}

    void incrementQueryEngineCounters(CurOp* curop) {
        auto& debug = curop->debug();
        const BSONObj& cmdObj = curop->opDescription();
        auto cmdName = cmdObj.firstElementFieldNameStringData();
        if (cmdName == "find" && debug.classicEngineUsed) {
            if (debug.classicEngineUsed.get()) {
                classicFindQueryCounter.increment();
            } else {
                sbeFindQueryCounter.increment();
            }
        } else if (cmdName == "aggregate" && debug.classicEngineUsed && debug.documentSourceUsed) {
            if (debug.classicEngineUsed.get()) {
                if (debug.documentSourceUsed.get()) {
                    classicHybridAggregationCounter.increment();
                } else {
                    classicOnlyAggregationCounter.increment();
                }
            } else {
                if (debug.documentSourceUsed.get()) {
                    sbeHybridAggregationCounter.increment();
                } else {
                    sbeOnlyAggregationCounter.increment();
                }
            }
        }
    }

    // Query counters that record whether a find query was fully or partially executed in SBE, or
    // fully executed using the classic engine. One or the other will always be incremented during a
    // query.
    Counter64 sbeFindQueryCounter;
    Counter64 classicFindQueryCounter;
    ServerStatusMetricField<Counter64> sbeFindQueryMetric;
    ServerStatusMetricField<Counter64> classicFindQueryMetric;
    // Aggregation query counters that record whether an aggregation was fully or partially executed
    // in DocumentSource (an sbe/classic hybrid plan), or fully pushed down to the sbe/classic
    // layer. Only incremented during aggregations.
    Counter64 sbeOnlyAggregationCounter;
    Counter64 classicOnlyAggregationCounter;
    Counter64 sbeHybridAggregationCounter;
    Counter64 classicHybridAggregationCounter;
    ServerStatusMetricField<Counter64> sbeOnlyAggregationMetric;
    ServerStatusMetricField<Counter64> classicOnlyAggregationMetric;
    ServerStatusMetricField<Counter64> sbeHybridAggregationMetric;
    ServerStatusMetricField<Counter64> classicHybridAggregationMetric;
};
extern QueryEngineCounters queryEngineCounters;

class OperatorCountersAggExpressions {
private:
    struct AggExprCounter {
        AggExprCounter(StringData name)
            : metric("operatorCounters.expressions." + name, &counter) {}

        Counter64 counter;
        ServerStatusMetricField<Counter64> metric;
    };

public:
    void addAggExpressionCounter(StringData name) {
        operatorCountersAggExpressionMap[name] = std::make_unique<AggExprCounter>(name);
    }

    void mergeCounters(StringMap<uint64_t>& toMerge) {
        for (auto&& [name, cnt] : toMerge) {
            if (auto it = operatorCountersAggExpressionMap.find(name);
                it != operatorCountersAggExpressionMap.end()) {
                it->second->counter.increment(cnt);
            }
        }
    }

private:
    // Map of aggregation expressions to the number of occurrences in aggregation pipelines.
    StringMap<std::unique_ptr<AggExprCounter>> operatorCountersAggExpressionMap = {};
};

extern OperatorCountersAggExpressions operatorCountersAggExpressions;

/**
 * Global counters for match expressions.
 */
class OperatorCountersMatchExpressions {
private:
    struct MatchExprCounter {
        MatchExprCounter(StringData name) : metric("operatorCounters.match." + name, &counter) {}

        Counter64 counter;
        ServerStatusMetricField<Counter64> metric;
    };

public:
    void addMatchExprCounter(StringData name) {
        operatorCountersMatchExprMap[name] = std::make_unique<MatchExprCounter>(name);
    }

    void mergeCounters(StringMap<uint64_t>& toMerge) {
        for (auto&& [name, cnt] : toMerge) {
            if (auto it = operatorCountersMatchExprMap.find(name);
                it != operatorCountersMatchExprMap.end()) {
                it->second->counter.increment(cnt);
            }
        }
    }

private:
    // Map of match expressions to the number of occurrences in queries.
    StringMap<std::unique_ptr<MatchExprCounter>> operatorCountersMatchExprMap = {};
};

extern OperatorCountersMatchExpressions operatorCountersMatchExpressions;
}  // namespace mongo
