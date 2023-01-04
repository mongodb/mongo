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

#include "mongo/db/query/telemetry.h"

#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/rate_limiting.h"
#include "mongo/db/query/telemetry_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/system_clock_source.h"
#include <optional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace telemetry {

bool isTelemetryEnabled() {
    return feature_flags::gFeatureFlagTelemetry.isEnabledAndIgnoreFCV();
}


namespace {

/**
 * Cap the telemetry store size.
 */
size_t capTelemetryStoreSize(size_t requestedSize) {
    size_t cappedStoreSize = memory_util::capMemorySize(
        requestedSize /*requestedSizeBytes*/, 1 /*maximumSizeGB*/, 25 /*percentTotalSystemMemory*/);
    // If capped size is less than requested size, the telemetry store has been capped at its
    // upper limit.
    if (cappedStoreSize < requestedSize) {
        LOGV2_DEBUG(7106502,
                    1,
                    "The telemetry store size has been capped",
                    "cappedSize"_attr = cappedStoreSize);
    }
    return cappedStoreSize;
}

/**
 * Get the telemetry store size based on the query job's value.
 */
size_t getTelemetryStoreSize() {
    auto status = memory_util::MemorySize::parse(queryTelemetryStoreSize.get());
    uassertStatusOK(status);
    size_t requestedSize = memory_util::getRequestedMemSizeInBytes(status.getValue());
    return capTelemetryStoreSize(requestedSize);
}

/**
 * A manager for the telemetry store allows a "pointer swap" on the telemetry store itself. The
 * usage patterns are as follows:
 *
 * - Updating the telemetry store uses the `getTelemetryStore()` method. The telemetry store
 *   instance is obtained, entries are looked up and mutated, or created anew.
 * - The telemetry store is "reset". This involves atomically allocating a new instance, once
 * there are no more updaters (readers of the store "pointer"), and returning the existing
 * instance.
 */
class TelemetryStoreManager {
public:
    template <typename... TelemetryStoreArgs>
    TelemetryStoreManager(TelemetryStoreArgs... args)
        : _telemetryStore(
              std::make_unique<TelemetryStore>(std::forward<TelemetryStoreArgs>(args)...)) {}

    /**
     * Acquire the instance of the telemetry store.
     */
    TelemetryStore& getTelemetryStore() {
        return *_telemetryStore;
    }

private:
    std::unique_ptr<TelemetryStore> _telemetryStore;
};

const auto telemetryStoreDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<TelemetryStoreManager>>();

const auto telemetryRateLimiter =
    ServiceContext::declareDecoration<std::unique_ptr<RateLimiting>>();

class TelemetryOnParamChangeUpdaterImpl final : public telemetry_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) final {
        auto requestedSize = memory_util::getRequestedMemSizeInBytes(memSize);
        auto cappedSize = capTelemetryStoreSize(requestedSize);

        auto& telemetryStoreManager = telemetryStoreDecoration(serviceCtx);
        auto&& telemetryStore = telemetryStoreManager->getTelemetryStore();
        telemetryStore.reset(cappedSize);
    }
};

ServiceContext::ConstructorActionRegisterer telemetryStoreManagerRegisterer{
    "TelemetryStoreManagerRegisterer", [](ServiceContext* serviceCtx) {
        if (!isTelemetryEnabled()) {
            // featureFlags are not allowed to be changed at runtime. Therefore it's not an issue
            // to not create a telemetry store in ConstructorActionRegisterer at start up with the
            // flag off - because the flag can not be turned on at any point afterwards.
            return;
        }

        telemetry_util::telemetryStoreOnParamChangeUpdater(serviceCtx) =
            std::make_unique<TelemetryOnParamChangeUpdaterImpl>();
        size_t size = getTelemetryStoreSize();
        auto&& globalTelemetryStoreManager = telemetryStoreDecoration(serviceCtx);
        // Many partitions reduces lock contention on both reading and write telemetry data.
        size_t numPartitions = 1024;
        size_t partitionBytes = size / numPartitions;
        size_t metricsSize = sizeof(TelemetryMetrics);
        if (partitionBytes < metricsSize * 10) {
            numPartitions = size / metricsSize;
            if (numPartitions < 1) {
                numPartitions = 1;
            }
        }
        globalTelemetryStoreManager = std::make_unique<TelemetryStoreManager>(size, numPartitions);
        telemetryRateLimiter(serviceCtx) =
            std::make_unique<RateLimiting>(queryTelemetrySamplingRate.load());
    }};

/**
 * Internal check for whether we should collect metrics. This checks the rate limiting
 * configuration for a global on/off decision and, if enabled, delegates to the rate limiter.
 */
bool shouldCollect(const ServiceContext* serviceCtx) {
    // Quick escape if telemetry is turned off.
    if (!isTelemetryEnabled()) {
        return false;
    }
    // Cannot collect telemetry if sampling rate is not greater than 0.
    if (telemetryRateLimiter(serviceCtx)->getSamplingRate() <= 0) {
        return false;
    }
    // Check if rate limiting allows us to collect telemetry for this request.
    if (!telemetryRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        return false;
    }
    return true;
}

/**
 * Add a field to the find op's telemetry key. The `value` will be redacted.
 */
void addToFindKey(BSONObjBuilder& builder, const StringData& fieldName, const BSONObj& value) {
    serializeBSONWhenNotEmpty(value.redact(false), fieldName, &builder);
}

/**
 * Recognize FLE payloads in a query and throw an exception if found.
 */
void throwIfEncounteringFLEPayload(const BSONElement& e) {
    constexpr auto safeContentLabel = "__safeContent__"_sd;
    constexpr auto fieldpath = "$__safeContent__"_sd;
    if (e.type() == BSONType::Object) {
        auto fieldname = e.fieldNameStringData();
        uassert(ErrorCodes::EncounteredFLEPayloadWhileRedacting,
                "Encountered __safeContent__, or an $_internalFle operator, which indicate a "
                "rewritten FLE2 query.",
                fieldname != safeContentLabel && !fieldname.startsWith("$_internalFle"_sd));
    } else if (e.type() == BSONType::String) {
        auto val = e.valueStringData();
        uassert(ErrorCodes::EncounteredFLEPayloadWhileRedacting,
                "Encountered $__safeContent__ fieldpath, which indicates a rewritten FLE2 query.",
                val != fieldpath);
    } else if (e.type() == BSONType::BinData && e.isBinData(BinDataType::Encrypt)) {
        int len;
        auto data = e.binData(len);
        uassert(ErrorCodes::EncounteredFLEPayloadWhileRedacting,
                "FLE1 Payload encountered in expression.",
                len > 1 && data[1] != char(EncryptedBinDataType::kDeterministic));
    }
}

/**
 * Get the metrics for a given key holding the appropriate locks.
 */
class LockedMetrics {
    LockedMetrics(TelemetryMetrics* metrics,
                  TelemetryStore& telemetryStore,
                  TelemetryStore::Partition partitionLock)
        : _metrics(metrics),
          _telemetryStore(telemetryStore),
          _partitionLock(std::move(partitionLock)) {}

public:
    static LockedMetrics get(OperationContext* opCtx, const BSONObj& telemetryKey) {
        auto&& telemetryStore = getTelemetryStore(opCtx);
        auto&& [statusWithMetrics, partitionLock] =
            telemetryStore.getWithPartitionLock(telemetryKey);
        TelemetryMetrics* metrics;
        if (statusWithMetrics.isOK()) {
            metrics = statusWithMetrics.getValue();
        } else {
            telemetryStore.put(telemetryKey, {}, partitionLock);
            auto newMetrics = partitionLock->get(telemetryKey);
            // This can happen if the budget is immediately exceeded. Specifically if the there is
            // not enough room for a single new entry if the number of partitions is too high
            // relative to the size.
            tassert(7064700, "Should find telemetry store entry", newMetrics.isOK());
            metrics = &newMetrics.getValue()->second;
        }
        return LockedMetrics{metrics, telemetryStore, std::move(partitionLock)};
    }

    TelemetryMetrics* operator->() const {
        return _metrics;
    }

private:
    TelemetryMetrics* _metrics;

    TelemetryStore& _telemetryStore;

    TelemetryStore::Partition _partitionLock;
};

/**
 * Upon reading telemetry data, we redact some keys. This is the list. See
 * TelemetryMetrics::redactKey().
 */
const stdx::unordered_set<std::string> kKeysToRedact = {"pipeline", "find"};

std::string sha256FieldNameHasher(const BSONElement& e) {
    auto&& fieldName = e.fieldNameStringData();
    auto hash = SHA256Block::computeHash({ConstDataRange(fieldName.rawData(), fieldName.size())});
    return hash.toString().substr(0, 12);
}

std::string constantFieldNameHasher(const BSONElement& e) {
    return {"###"};
}

/**
 * Admittedly an abuse of the BSON redaction interface, we recognize FLE payloads here and avoid
 * collecting telemetry for the query.
 */
std::string fleSafeFieldNameRedactor(const BSONElement& e) {
    throwIfEncounteringFLEPayload(e);
    // Ideally we would change interface to avoid copying here.
    return e.fieldNameStringData().toString();
}

/**
 * Append the element to the builder and redact any literals within the element. The element may be
 * of any type.
 */
void appendWithRedactedLiterals(BSONObjBuilder& builder, const BSONElement& el) {
    if (el.type() == Object) {
        builder.append(el.fieldNameStringData(), el.Obj().redact(false, fleSafeFieldNameRedactor));
    } else if (el.type() == Array) {
        BSONObjBuilder arrayBuilder = builder.subarrayStart(fleSafeFieldNameRedactor(el));
        for (auto&& arrayElem : el.Obj()) {
            appendWithRedactedLiterals(arrayBuilder, arrayElem);
        }
        arrayBuilder.done();
    } else {
        auto fieldName = fleSafeFieldNameRedactor(el);
        builder.append(fieldName, "###"_sd);
    }
    builder.done();
}

}  // namespace

const BSONObj& TelemetryMetrics::redactKey(const BSONObj& key, bool redactFieldNames) const {
    // The redacted key for each entry is cached on first computation. However, if the redaction
    // straegy has flipped (from no redaction to SHA256, vice versa), we just return the key passed
    // to the function, so entries returned to the user match the redaction strategy requested in
    // the most recent telemetry command.
    if (!redactFieldNames) {
        return key;
    }
    if (_redactedKey) {
        return *_redactedKey;
    }
    // The telemetry key is of the following form:
    // { "<CMD_TYPE>": {...}, "namespace": "...", "applicationName": "...", ... }
    //
    // The part of the key we need to redact is the object in the <CMD_TYPE> element. In the case of
    // an aggregate() command, it will look something like:
    // > "pipeline" : [ { "$telemetry" : {} },
    //					{ "$addFields" : { "x" : { "$someExpr" {} } } } ],
    // We should preserve the top-level stage names in the pipeline but redact all field names of
    // children.
    //
    // The find-specific key will look like so:
    // > "find" : { "find" : "###", "filter" : { "_id" : { "$ne" : "###" } } },
    // Again, we should preserve the top-level keys and redact all field names of children.
    BSONObjBuilder redacted;
    for (BSONElement e : key) {
        if ((e.type() == Object || e.type() == Array) &&
            kKeysToRedact.count(e.fieldNameStringData().toString()) == 1) {
            auto redactor = [&](BSONObjBuilder subObj, const BSONObj& obj) {
                for (BSONElement e2 : obj) {
                    if (e2.type() == Object) {
                        // Sha256 redaction strategy.
                        subObj.append(e2.fieldNameStringData(),
                                      e2.Obj().redact(false, sha256FieldNameHasher));
                    } else {
                        subObj.append(e2);
                    }
                }
                subObj.done();
            };

            // Now we're inside the <CMD_TYPE>:{} entry and want to preserve the top-level field
            // names. If it's a [pipeline] array, we redact each element in isolation.
            if (e.type() == Object) {
                redactor(redacted.subobjStart(e.fieldNameStringData()), e.Obj());
            } else {
                BSONObjBuilder subArr = redacted.subarrayStart(e.fieldNameStringData());
                for (BSONElement stage : e.Obj()) {
                    redactor(subArr.subobjStart(""), stage.Obj());
                }
            }
        } else {
            redacted.append(e);
        }
    }
    _redactedKey = redacted.obj();
    return *_redactedKey;
}

void registerAggRequest(const AggregateCommandRequest& request, OperationContext* opCtx) {

    if (!isTelemetryEnabled()) {
        return;
    }

    if (request.getEncryptionInformation()) {
        return;
    }

    // Queries against metadata collections should never appear in telemetry data.
    if (request.getNamespace().isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }

    BSONObjBuilder telemetryKey;
    BSONObjBuilder pipelineBuilder = telemetryKey.subarrayStart("pipeline"_sd);
    try {
        for (auto&& stage : request.getPipeline()) {
            BSONObjBuilder stageBuilder = pipelineBuilder.subobjStart("stage"_sd);
            appendWithRedactedLiterals(stageBuilder, stage.firstElement());
        }
        pipelineBuilder.done();
        telemetryKey.append("namespace", request.getNamespace().toString());
        if (request.getReadConcern()) {
            telemetryKey.append("readConcern", *request.getReadConcern());
        }
        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            telemetryKey.append("applicationName", metadata->getApplicationName());
        }
    } catch (ExceptionFor<ErrorCodes::EncounteredFLEPayloadWhileRedacting>&) {
        return;
    }
    opCtx->storeQueryBSON(telemetryKey.obj());
    // Management of the telemetry key works as follows.
    //
    // Query execution potentially spans more than one request/operation. For this reason, we need a
    // mechanism to communicate the context (the telemetry key) across operations on the same query.
    // In order to accomplish this, we store the telemetry key in the plan explainer which exists
    // for the entire life of the query.
    //
    // - Telemetry key must be stored in the OperationContext before the PlanExecutor is created.
    //   This is accomplished by calling registerXXXRequest() in run_aggregate.cpp and
    //   find_cmd.cpp before the PlanExecutor is created.
    //
    // - During collectTelemetry(), the telemetry key is retrieved from the OperationContext to
    //   write metrics into the telemetry store. This is done at the end of the operation.
    //
    // - Upon getMore() calls, registerGetMoreRequest() copy the telemetry key from the
    //   PlanExplainer to the OperationContext.
}

void registerFindRequest(const FindCommandRequest& request,
                         const NamespaceString& collection,
                         OperationContext* opCtx) {
    if (!isTelemetryEnabled()) {
        return;
    }
    if (request.getEncryptionInformation()) {
        return;
    }

    // Queries against metadata collections should never appear in telemetry data.
    if (collection.isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }

    BSONObjBuilder telemetryKey;
    try {
        // Serialize the request.
        BSONObjBuilder serializedRequest;
        BSONObjBuilder asElement = serializedRequest.subobjStart("find");
        request.serialize({}, &asElement);
        asElement.done();
        // And append as an element to the telemetry key.
        appendWithRedactedLiterals(telemetryKey, serializedRequest.obj().firstElement());
        telemetryKey.append("namespace", collection.toString());
        if (request.getReadConcern()) {
            telemetryKey.append("readConcern", *request.getReadConcern());
        }
        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            telemetryKey.append("applicationName", metadata->getApplicationName());
        }
    } catch (ExceptionFor<ErrorCodes::EncounteredFLEPayloadWhileRedacting>&) {
        return;
    }
    opCtx->storeQueryBSON(telemetryKey.obj());
}

void registerGetMoreRequest(OperationContext* opCtx, const PlanExplainer& planExplainer) {
    if (!isTelemetryEnabled()) {
        return;
    }
    auto&& telemetryKey = planExplainer.getTelemetryKey();
    if (telemetryKey.isEmpty() || !shouldCollect(opCtx->getServiceContext())) {
        return;
    }
    opCtx->storeQueryBSON(telemetryKey);
}

TelemetryStore& getTelemetryStore(OperationContext* opCtx) {
    uassert(6579000, "Telemetry is not enabled without the feature flag on", isTelemetryEnabled());
    return telemetryStoreDecoration(opCtx->getServiceContext())->getTelemetryStore();
}

void recordExecution(OperationContext* opCtx, const OpDebug& opDebug, bool isFle) {

    if (!isTelemetryEnabled()) {
        return;
    }
    if (isFle) {
        return;
    }
    auto&& telemetryKey = opCtx->getTelemetryKey();
    if (telemetryKey.isEmpty()) {
        return;
    }
    auto&& metrics = LockedMetrics::get(opCtx, telemetryKey);
    metrics->execCount++;
    metrics->queryOptMicros.aggregate(opDebug.planningTime.count());
}

void collectTelemetry(OperationContext* opCtx, const OpDebug& opDebug) {
    auto&& telemetryKey = opCtx->getTelemetryKey();
    if (telemetryKey.isEmpty()) {
        return;
    }
    auto&& metrics = LockedMetrics::get(opCtx, telemetryKey);
    metrics->docsReturned.aggregate(opDebug.nreturned);
    metrics->docsScanned.aggregate(opDebug.additiveMetrics.docsExamined.value_or(0));
    metrics->keysScanned.aggregate(opDebug.additiveMetrics.keysExamined.value_or(0));
    metrics->lastExecutionMicros = opDebug.executionTime.count();
    metrics->queryExecMicros.aggregate(opDebug.executionTime.count());
}
}  // namespace telemetry
}  // namespace mongo
