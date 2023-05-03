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
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/find_command_gen.h"
// TODO SERVER-76557 remove include of find_request_shapifier
#include "mongo/db/query/find_request_shapifier.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/rate_limiting.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/query/telemetry_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"
#include "query_shape.h"
#include <optional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace telemetry {

/**
 * Redacts all BSONObj field names as if they were paths, unless the field name is a special hint
 * operator.
 */
namespace {

boost::optional<std::string> getApplicationName(const OperationContext* opCtx) {
    if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
        return metadata->getApplicationName().toString();
    }
    return boost::none;
}
}  // namespace

// TODO SERVER-76557 can remove this makeTelemetryKey
BSONObj makeTelemetryKey(const FindCommandRequest& findCommand,
                         const SerializationOptions& opts,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         boost::optional<const TelemetryMetrics&> existingMetrics) {

    BSONObjBuilder bob;
    bob.append("queryShape", query_shape::extractQueryShape(findCommand, opts, expCtx));
    if (auto optObj = findCommand.getReadConcern()) {
        // Read concern should not be considered a literal.
        bob.append(FindCommandRequest::kReadConcernFieldName, optObj.get());
    }
    auto appName = [&]() -> boost::optional<std::string> {
        if (existingMetrics.has_value()) {
            if (existingMetrics->applicationName.has_value()) {
                return existingMetrics->applicationName;
            }
        } else {
            if (auto appName = getApplicationName(expCtx->opCtx)) {
                return appName.value();
            }
        }
        return boost::none;
    }();
    if (appName.has_value()) {
        bob.append("applicationName", opts.serializeIdentifier(appName.value()));
    }
    return bob.obj();
}

CounterMetric telemetryStoreSizeEstimateBytesMetric("telemetry.telemetryStoreSizeEstimateBytes");

namespace {

CounterMetric telemetryEvictedMetric("telemetry.numEvicted");
CounterMetric telemetryRateLimitedRequestsMetric("telemetry.numRateLimitedRequests");
CounterMetric telemetryStoreWriteErrorsMetric("telemetry.numTelemetryStoreWriteErrors");

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
    size_t requestedSize = memory_util::convertToSizeInBytes(status.getValue());
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
    TelemetryStoreManager(size_t cacheSize, size_t numPartitions)
        : _telemetryStore(std::make_unique<TelemetryStore>(cacheSize, numPartitions)),
          _maxSize(cacheSize) {}

    /**
     * Acquire the instance of the telemetry store.
     */
    TelemetryStore& getTelemetryStore() {
        return *_telemetryStore;
    }

    size_t getMaxSize() {
        return _maxSize;
    }

    /**
     * Resize the telemetry store and return the number of evicted
     * entries.
     */
    size_t resetSize(size_t cacheSize) {
        _maxSize = cacheSize;
        return _telemetryStore->reset(cacheSize);
    }

private:
    std::unique_ptr<TelemetryStore> _telemetryStore;

    /**
     * Max size of the telemetry store. Tracked here to avoid having to recompute after it's divided
     * up into partitions.
     */
    size_t _maxSize;
};

const auto telemetryStoreDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<TelemetryStoreManager>>();

const auto telemetryRateLimiter =
    ServiceContext::declareDecoration<std::unique_ptr<RateLimiting>>();

class TelemetryOnParamChangeUpdaterImpl final : public telemetry_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) final {
        auto requestedSize = memory_util::convertToSizeInBytes(memSize);
        auto cappedSize = capTelemetryStoreSize(requestedSize);
        auto& telemetryStoreManager = telemetryStoreDecoration(serviceCtx);
        size_t numEvicted = telemetryStoreManager->resetSize(cappedSize);
        telemetryEvictedMetric.increment(numEvicted);
    }

    void updateSamplingRate(ServiceContext* serviceCtx, int samplingRate) {
        telemetryRateLimiter(serviceCtx).get()->setSamplingRate(samplingRate);
    }
};

ServiceContext::ConstructorActionRegisterer telemetryStoreManagerRegisterer{
    "TelemetryStoreManagerRegisterer", [](ServiceContext* serviceCtx) {
        // It is possible that this is called before FCV is properly set up. Setting up the store if
        // the flag is enabled but FCV is incorrect is safe, and guards against the FCV being
        // changed to a supported version later.
        if (!feature_flags::gFeatureFlagTelemetry.isEnabledAndIgnoreFCVUnsafeAtStartup()) {
            // featureFlags are not allowed to be changed at runtime. Therefore it's not an issue
            // to not create a telemetry store in ConstructorActionRegisterer at start up with the
            // flag off - because the flag can not be turned on at any point afterwards.
            telemetry_util::telemetryStoreOnParamChangeUpdater(serviceCtx) =
                std::make_unique<telemetry_util::NoChangesAllowedTelemetryParamUpdater>();
            return;
        }

        telemetry_util::telemetryStoreOnParamChangeUpdater(serviceCtx) =
            std::make_unique<TelemetryOnParamChangeUpdaterImpl>();
        size_t size = getTelemetryStoreSize();
        auto&& globalTelemetryStoreManager = telemetryStoreDecoration(serviceCtx);
        // The plan cache and telemetry store should use the same number of partitions.
        // That is, the number of cpu cores.
        size_t numPartitions = ProcessInfo::getNumCores();
        size_t partitionBytes = size / numPartitions;
        size_t metricsSize = sizeof(TelemetryMetrics);
        if (partitionBytes < metricsSize * 10) {
            numPartitions = size / metricsSize;
            if (numPartitions < 1) {
                numPartitions = 1;
            }
        }
        globalTelemetryStoreManager = std::make_unique<TelemetryStoreManager>(size, numPartitions);
        auto configuredSamplingRate = queryTelemetrySamplingRate.load();
        telemetryRateLimiter(serviceCtx) = std::make_unique<RateLimiting>(
            configuredSamplingRate < 0 ? INT_MAX : configuredSamplingRate);
    }};

/**
 * Top-level checks for whether telemetry collection is enabled. If this returns false, we must go
 * no further.
 */
bool isTelemetryEnabled(const ServiceContext* serviceCtx) {
    // During initialization FCV may not yet be setup but queries could be run. We can't
    // check whether telemetry should be enabled without FCV, so default to not recording
    // those queries.
    // TODO SERVER-75935 Remove FCV Check.
    return feature_flags::gFeatureFlagTelemetry.isEnabled(
               serverGlobalParams.featureCompatibility) &&
        telemetryStoreDecoration(serviceCtx)->getMaxSize() > 0;
}

/**
 * Internal check for whether we should collect metrics. This checks the rate limiting
 * configuration for a global on/off decision and, if enabled, delegates to the rate limiter.
 */
bool shouldCollect(const ServiceContext* serviceCtx) {
    // Quick escape if telemetry is turned off.
    if (!isTelemetryEnabled(serviceCtx)) {
        return false;
    }
    // Cannot collect telemetry if sampling rate is not greater than 0. Note that we do not
    // increment telemetryRateLimitedRequestsMetric here since telemetry is entirely disabled.
    if (telemetryRateLimiter(serviceCtx)->getSamplingRate() <= 0) {
        return false;
    }
    // Check if rate limiting allows us to collect telemetry for this request.
    if (telemetryRateLimiter(serviceCtx)->getSamplingRate() < INT_MAX &&
        !telemetryRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        telemetryRateLimitedRequestsMetric.increment();
        return false;
    }
    return true;
}

/**
 * Add a field to the find op's telemetry key. The `value` will have hmac applied.
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
        uassert(ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac,
                "Encountered __safeContent__, or an $_internalFle operator, which indicate a "
                "rewritten FLE2 query.",
                fieldname != safeContentLabel && !fieldname.startsWith("$_internalFle"_sd));
    } else if (e.type() == BSONType::String) {
        auto val = e.valueStringData();
        uassert(ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac,
                "Encountered $__safeContent__ fieldpath, which indicates a rewritten FLE2 query.",
                val != fieldpath);
    } else if (e.type() == BSONType::BinData && e.isBinData(BinDataType::Encrypt)) {
        int len;
        auto data = e.binData(len);
        uassert(ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac,
                "FLE1 Payload encountered in expression.",
                len > 1 && data[1] != char(EncryptedBinDataType::kDeterministic));
    }
}

/**
 * Upon reading telemetry data, we apply hmac to some keys. This is the list. See
 * TelemetryMetrics::applyHmacToKey().
 */
const stdx::unordered_set<std::string> kKeysToApplyHmac = {"pipeline", "find"};

std::string sha256HmacStringDataHasher(std::string key, const StringData& sd) {
    auto hashed = SHA256Block::computeHmac(
        (const uint8_t*)key.data(), key.size(), (const uint8_t*)sd.rawData(), sd.size());
    return hashed.toString();
}

std::string sha256HmacFieldNameHasher(std::string key, const BSONElement& e) {
    auto&& fieldName = e.fieldNameStringData();
    return sha256HmacStringDataHasher(key, fieldName);
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
 * Append the element to the builder and apply hmac to any literals within the element. The element
 * may be of any type.
 */
void appendWithHmacAppliedLiterals(BSONObjBuilder& builder, const BSONElement& el) {
    if (el.type() == Object) {
        builder.append(el.fieldNameStringData(), el.Obj().redact(false, fleSafeFieldNameRedactor));
    } else if (el.type() == Array) {
        BSONObjBuilder arrayBuilder = builder.subarrayStart(fleSafeFieldNameRedactor(el));
        for (auto&& arrayElem : el.Obj()) {
            appendWithHmacAppliedLiterals(arrayBuilder, arrayElem);
        }
        arrayBuilder.done();
    } else {
        auto fieldName = fleSafeFieldNameRedactor(el);
        builder.append(fieldName, "###"_sd);
    }
}

static const StringData replacementForLiteralArgs = "?"_sd;

}  // namespace

BSONObj TelemetryMetrics::applyHmacToKey(const BSONObj& key,
                                         bool applyHmacToIdentifiers,
                                         std::string hmacKey,
                                         OperationContext* opCtx) const {
    if (!applyHmacToIdentifiers) {
        return key;
    }

    if (_hmacAppliedKey) {
        return *_hmacAppliedKey;
    }
    // The telemetry key for find queries is generated by serializing all the command fields
    // and applied hmac if SerializationOptions indicate to do so. The resulting key is of the form:
    // {
    //    queryShape: {
    //        cmdNs: {db: "...", coll: "..."},
    //        find: "...",
    //        filter: {"...": {"$eq": "?number"}},
    //    },
    //    applicationName: kHashedApplicationName
    // }
    // queryShape may include additional fields, eg hint, limit sort, etc, depending on the original
    // query.
    if (cmdObj.hasField(FindCommandRequest::kCommandName)) {
        tassert(7198600, "Find command must have a namespace string.", this->nss.nss().has_value());
        auto findCommand =
            query_request_helper::makeFromFindCommand(cmdObj, this->nss.nss().value(), false);
        auto nss = findCommand->getNamespaceOrUUID().nss();
        uassert(7349400, "Namespace must be defined", nss.has_value());

        auto serializationOpts = applyHmacToIdentifiers
            ? SerializationOptions(
                  [&](StringData sd) { return sha256HmacStringDataHasher(hmacKey, sd); },
                  LiteralSerializationPolicy::kToDebugTypeString)
            : SerializationOptions(false);

        auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                        *findCommand,
                                                        nullptr /* collator doesn't matter here.*/,
                                                        false /* mayDbProfile */);
        expCtx->maxFeatureCompatibilityVersion = boost::none;  // Ensure all features are allowed.
        expCtx->stopExpressionCounters();

        // TODO SERVER-76557 call makeTelemetryKey thru FindRequestShapifier kept in telemetry store
        auto key = makeTelemetryKey(*findCommand, serializationOpts, expCtx, *this);
        // TODO: SERVER-76526 as part of this ticket, no form of the key (hmac applied or not) will
        // be cached with TelemetryMetrics.
        if (applyHmacToIdentifiers) {
            _hmacAppliedKey = key;
            return *_hmacAppliedKey;
        }
        return key;
    }

    // The telemetry key for agg queries is of the following form:
    // { "agg": {...}, "namespace": "...", "applicationName": "...", ... }
    //
    // The part of the key we need to apply hmac to is the object in the <CMD_TYPE> element. In the
    // case of an aggregate() command, it will look something like: > "pipeline" : [ { "$telemetry"
    // : {} },
    //					{ "$addFields" : { "x" : { "$someExpr" {} } } } ],
    // We should preserve the top-level stage names in the pipeline but apply hmac to all field
    // names of children.
    BSONObjBuilder hmacAppliedBuilder;
    for (BSONElement e : key) {
        if ((e.type() == Object || e.type() == Array) &&
            kKeysToApplyHmac.count(e.fieldNameStringData().toString()) == 1) {
            auto hmacApplicator = [&](BSONObjBuilder subObj, const BSONObj& obj) {
                for (BSONElement e2 : obj) {
                    if (e2.type() == Object) {
                        subObj.append(e2.fieldNameStringData(),
                                      e2.Obj().redact(false, [&](const BSONElement& e) {
                                          return sha256HmacFieldNameHasher(hmacKey, e);
                                      }));
                    } else {
                        subObj.append(e2);
                    }
                }
                subObj.done();
            };

            // Now we're inside the <CMD_TYPE>:{} entry and want to preserve the top-level field
            // names. If it's a [pipeline] array, we redact each element in isolation.
            if (e.type() == Object) {
                hmacApplicator(hmacAppliedBuilder.subobjStart(e.fieldNameStringData()), e.Obj());
            } else {
                BSONObjBuilder subArr = hmacAppliedBuilder.subarrayStart(e.fieldNameStringData());
                for (BSONElement stage : e.Obj()) {
                    hmacApplicator(subArr.subobjStart(""), stage.Obj());
                }
            }
        } else {
            hmacAppliedBuilder.append(e);
        }
    }
    _hmacAppliedKey = hmacAppliedBuilder.obj();
    return *_hmacAppliedKey;
}

// The originating command/query does not persist through the end of query execution. In order to
// pair the telemetry metrics that are collected at the end of execution with the original query, it
// is necessary to register the original query during planning and persist it after
// execution.

// During planning, registerRequest is called to serialize the query shape and context (together,
// the telemetry context) and save it to OpDebug. Moreover, as query execution may span more than
// one request/operation and OpDebug does not persist through cursor iteration, it is necessary to
// communicate the telemetry context across operations. In this way, the telemetry context is
// registered to the cursor, so upon getMore() calls, the cursor manager passes the telemetry key
// from the pinned cursor to the new OpDebug.

// Once query execution is complete, the telemetry context is grabbed from OpDebug, a telemetry key
// is generated from this and metrics are paired to this key in the telemetry store.
void registerAggRequest(const AggregateCommandRequest& request, OperationContext* opCtx) {
    if (!isTelemetryEnabled(opCtx->getServiceContext())) {
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
            appendWithHmacAppliedLiterals(stageBuilder, stage.firstElement());
            stageBuilder.done();
        }
        pipelineBuilder.done();
        telemetryKey.append("namespace", request.getNamespace().toString());
        if (request.getReadConcern()) {
            telemetryKey.append("readConcern", *request.getReadConcern());
        }
        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            telemetryKey.append("applicationName", metadata->getApplicationName());
        }
    } catch (ExceptionFor<ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac>&) {
        return;
    }

    CurOp::get(opCtx)->debug().telemetryStoreKey = telemetryKey.obj();
}

void registerRequest(const RequestShapifier& requestShapifier,
                     const NamespaceString& collection,
                     OperationContext* opCtx,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!isTelemetryEnabled(opCtx->getServiceContext())) {
        return;
    }

    // Queries against metadata collections should never appear in telemetry data.
    if (collection.isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }

    SerializationOptions options;
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    options.replacementForLiteralArgs = replacementForLiteralArgs;
    CurOp::get(opCtx)->debug().telemetryStoreKey =
        requestShapifier.makeTelemetryKey(options, expCtx);
}

TelemetryStore& getTelemetryStore(OperationContext* opCtx) {
    uassert(6579000,
            "Telemetry is not enabled without the feature flag on and a cache size greater than 0 "
            "bytes",
            isTelemetryEnabled(opCtx->getServiceContext()));
    return telemetryStoreDecoration(opCtx->getServiceContext())->getTelemetryStore();
}

void writeTelemetry(OperationContext* opCtx,
                    boost::optional<BSONObj> telemetryKey,
                    const BSONObj& cmdObj,
                    const uint64_t queryExecMicros,
                    const uint64_t docsReturned) {
    if (!telemetryKey) {
        return;
    }
    auto&& telemetryStore = getTelemetryStore(opCtx);
    auto&& [statusWithMetrics, partitionLock] = telemetryStore.getWithPartitionLock(*telemetryKey);
    std::shared_ptr<TelemetryMetrics> metrics;
    if (statusWithMetrics.isOK()) {
        metrics = *statusWithMetrics.getValue();
    } else {
        size_t numEvicted =
            telemetryStore.put(*telemetryKey,
                               std::make_shared<TelemetryMetrics>(
                                   cmdObj, getApplicationName(opCtx), CurOp::get(opCtx)->getNSS()),
                               partitionLock);
        telemetryEvictedMetric.increment(numEvicted);
        auto newMetrics = partitionLock->get(*telemetryKey);
        if (!newMetrics.isOK()) {
            // This can happen if the budget is immediately exceeded. Specifically if the there is
            // not enough room for a single new entry if the number of partitions is too high
            // relative to the size.
            telemetryStoreWriteErrorsMetric.increment();
            LOGV2_DEBUG(7560900,
                        1,
                        "Failed to store telemetry entry.",
                        "status"_attr = newMetrics.getStatus(),
                        "rawKey"_attr = redact(*telemetryKey));
            return;
        }
        metrics = newMetrics.getValue()->second;
    }

    metrics->lastExecutionMicros = queryExecMicros;
    metrics->execCount++;
    metrics->queryExecMicros.aggregate(queryExecMicros);
    metrics->docsReturned.aggregate(docsReturned);
}

void collectMetricsOnOpDebug(CurOp* curOp, long long nreturned) {
    auto&& opDebug = curOp->debug();
    opDebug.additiveMetrics.nreturned = nreturned;
    // executionTime is set with the final executionTime in CurOp::completeAndLogOperation, but for
    // telemetry collection we want it set before incrementing cursor metrics using AdditiveMetrics.
    // The value set here will be overwritten later in CurOp::completeAndLogOperation.
    opDebug.additiveMetrics.executionTime = curOp->elapsedTimeExcludingPauses();
}
}  // namespace telemetry
}  // namespace mongo
