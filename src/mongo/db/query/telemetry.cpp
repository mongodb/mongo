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
#include "mongo/db/query/find_command_gen.h"
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
#include <optional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace telemetry {

/**
 * Redacts all BSONObj field names as if they were paths, unless the field name is a special hint
 * operator.
 */
namespace {
static std::string hintSpecialField = "$hint";
void addLiteralFieldsWithRedaction(BSONObjBuilder* bob,
                                   const FindCommandRequest& findCommand,
                                   StringData newLiteral) {

    if (findCommand.getLimit()) {
        bob->append(FindCommandRequest::kLimitFieldName, newLiteral);
    }
    if (findCommand.getSkip()) {
        bob->append(FindCommandRequest::kSkipFieldName, newLiteral);
    }
    if (findCommand.getBatchSize()) {
        bob->append(FindCommandRequest::kBatchSizeFieldName, newLiteral);
    }
    if (findCommand.getMaxTimeMS()) {
        bob->append(FindCommandRequest::kMaxTimeMSFieldName, newLiteral);
    }
    if (findCommand.getNoCursorTimeout()) {
        bob->append(FindCommandRequest::kNoCursorTimeoutFieldName, newLiteral);
    }
}

void addLiteralFieldsWithoutRedaction(BSONObjBuilder* bob, const FindCommandRequest& findCommand) {
    if (auto param = findCommand.getLimit()) {
        bob->append(FindCommandRequest::kLimitFieldName, param.get());
    }
    if (auto param = findCommand.getSkip()) {
        bob->append(FindCommandRequest::kSkipFieldName, param.get());
    }
    if (auto param = findCommand.getBatchSize()) {
        bob->append(FindCommandRequest::kBatchSizeFieldName, param.get());
    }
    if (auto param = findCommand.getMaxTimeMS()) {
        bob->append(FindCommandRequest::kMaxTimeMSFieldName, param.get());
    }
    if (findCommand.getNoCursorTimeout().has_value()) {
        bob->append(FindCommandRequest::kNoCursorTimeoutFieldName,
                    findCommand.getNoCursorTimeout().value_or(false));
    }
}


static std::vector<
    std::pair<StringData, std::function<const OptionalBool(const FindCommandRequest&)>>>
    boolArgMap = {
        {FindCommandRequest::kSingleBatchFieldName, &FindCommandRequest::getSingleBatch},
        {FindCommandRequest::kAllowDiskUseFieldName, &FindCommandRequest::getAllowDiskUse},
        {FindCommandRequest::kReturnKeyFieldName, &FindCommandRequest::getReturnKey},
        {FindCommandRequest::kShowRecordIdFieldName, &FindCommandRequest::getShowRecordId},
        {FindCommandRequest::kTailableFieldName, &FindCommandRequest::getTailable},
        {FindCommandRequest::kAwaitDataFieldName, &FindCommandRequest::getAwaitData},
        {FindCommandRequest::kAllowPartialResultsFieldName,
         &FindCommandRequest::getAllowPartialResults},
        {FindCommandRequest::kMirroredFieldName, &FindCommandRequest::getMirrored},
};
std::vector<std::pair<StringData, std::function<const BSONObj(const FindCommandRequest&)>>>
    objArgMap = {
        {FindCommandRequest::kCollationFieldName, &FindCommandRequest::getCollation},

};

void addRemainingFindCommandFields(BSONObjBuilder* bob, const FindCommandRequest& findCommand) {
    for (auto [fieldName, getterFunction] : boolArgMap) {
        auto optBool = getterFunction(findCommand);
        if (optBool.has_value()) {
            bob->append(fieldName, optBool.value_or(false));
        }
    }
    if (auto optObj = findCommand.getReadConcern()) {
        bob->append(FindCommandRequest::kReadConcernFieldName, optObj.get());
    }
    auto collation = findCommand.getCollation();
    if (!collation.isEmpty()) {
        bob->append(FindCommandRequest::kCollationFieldName, collation);
    }
}
boost::optional<std::string> getApplicationName(const OperationContext* opCtx) {
    if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
        return metadata->getApplicationName().toString();
    }
    return boost::none;
}
}  // namespace
BSONObj redactHintComponent(BSONObj obj, const SerializationOptions& opts, bool redactValues) {
    BSONObjBuilder bob;
    for (BSONElement elem : obj) {
        if (hintSpecialField.compare(elem.fieldName()) == 0) {
            tassert(7421703,
                    "Hinted field must be a string with $hint operator",
                    elem.type() == BSONType::String);
            bob.append(hintSpecialField, opts.serializeFieldPathFromString(elem.String()));
            continue;
        }

        if (opts.replacementForLiteralArgs && redactValues) {
            bob.append(opts.serializeFieldPathFromString(elem.fieldName()),
                       opts.replacementForLiteralArgs.get());
        } else {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldName()));
        }
    }
    return bob.obj();
}

/**
 * In a let specification all field names are variable names, and all values are either expressions
 * or constants.
 */
BSONObj redactLetSpec(BSONObj letSpec,
                      const SerializationOptions& opts,
                      boost::intrusive_ptr<ExpressionContext> expCtx) {

    BSONObjBuilder bob;
    for (BSONElement elem : letSpec) {
        auto redactedValue =
            Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState)
                ->serialize(opts);
        // Note that this will throw on deeply nested let variables.
        redactedValue.addToBsonObj(&bob, opts.serializeFieldPathFromString(elem.fieldName()));
    }
    return bob.obj();
}

StatusWith<BSONObj> makeTelemetryKey(const FindCommandRequest& findCommand,
                                     const SerializationOptions& opts,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     boost::optional<const TelemetryMetrics&> existingMetrics) {
    // TODO: SERVER-75156 Factor query shape out of telemetry. That ticket will involve splitting
    // this function up and moving most of it to another, non-telemetry related header.

    if (!opts.redactIdentifiers && !opts.replacementForLiteralArgs) {
        // Short circuit if no redaction needs to be done.
        BSONObjBuilder bob;
        findCommand.serialize({}, &bob);
        return bob.obj();
    }

    // This function enumerates all the fields in a find command and either copies or attempts to
    // redact them.
    BSONObjBuilder bob;

    // Serialize the namespace as part of the query shape.
    {
        BSONObjBuilder cmdNs = bob.subobjStart("cmdNs");
        auto ns = findCommand.getNamespaceOrUUID();
        if (ns.nss()) {
            auto nss = ns.nss().value();
            if (nss.tenantId()) {
                cmdNs.append("tenantId",
                             opts.serializeIdentifier(nss.tenantId().value().toString()));
            }
            cmdNs.append("db", opts.serializeIdentifier(nss.db()));
            cmdNs.append("coll", opts.serializeIdentifier(nss.coll()));
        } else {
            cmdNs.append("uuid", opts.serializeIdentifier(ns.uuid()->toString()));
        }
        cmdNs.done();
    }

    // Redact the namespace of the command.
    {
        auto nssOrUUID = findCommand.getNamespaceOrUUID();
        std::string toSerialize;
        if (nssOrUUID.uuid()) {
            toSerialize = opts.serializeIdentifier(nssOrUUID.toString());
        } else {
            // Database is set at the command level, only serialize the collection here.
            toSerialize = opts.serializeIdentifier(nssOrUUID.nss()->coll());
        }
        bob.append(FindCommandRequest::kCommandName, toSerialize);
    }

    std::unique_ptr<MatchExpression> filterExpr;
    // Filter.
    {
        auto filter = findCommand.getFilter();
        auto filterParsed =
            MatchExpressionParser::parse(findCommand.getFilter(),
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        if (!filterParsed.isOK()) {
            return filterParsed.getStatus();
        }

        filterExpr = std::move(filterParsed.getValue());
        bob.append(FindCommandRequest::kFilterFieldName, filterExpr->serialize(opts));
    }

    // Let Spec.
    if (auto letSpec = findCommand.getLet()) {
        auto redactedObj = redactLetSpec(letSpec.get(), opts, expCtx);
        auto ownedObj = redactedObj.getOwned();
        bob.append(FindCommandRequest::kLetFieldName, std::move(ownedObj));
    }

    if (!findCommand.getProjection().isEmpty()) {
        // Parse to Projection
        auto projection =
            projection_ast::parseAndAnalyze(expCtx,
                                            findCommand.getProjection(),
                                            filterExpr.get(),
                                            findCommand.getFilter(),
                                            ProjectionPolicies::findProjectionPolicies());

        bob.append(FindCommandRequest::kProjectionFieldName,
                   projection_ast::serialize(projection, opts));
    }

    // Assume the hint is correct and contains field names. It is possible that this hint
    // doesn't actually represent an index, but we can't detect that here.
    // Hint, max, and min won't serialize if the object is empty.
    if (!findCommand.getHint().isEmpty()) {
        bob.append(FindCommandRequest::kHintFieldName,
                   redactHintComponent(findCommand.getHint(), opts, false));
        // Max/Min aren't valid without hint.
        if (!findCommand.getMax().isEmpty()) {
            bob.append(FindCommandRequest::kMaxFieldName,
                       redactHintComponent(findCommand.getMax(), opts, true));
        }
        if (!findCommand.getMin().isEmpty()) {
            bob.append(FindCommandRequest::kMinFieldName,
                       redactHintComponent(findCommand.getMin(), opts, true));
        }
    }

    // Sort.
    {
        auto sortSpec = findCommand.getSort();
        if (!sortSpec.isEmpty()) {
            auto sort = SortPattern(sortSpec, expCtx);
            bob.append(
                FindCommandRequest::kSortFieldName,
                sort.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
                    .toBson());
        }
    }

    // Fields for literal redaction. Adds limit, skip, batchSize, maxTimeMS, and noCursorTimeOut
    if (opts.replacementForLiteralArgs) {
        addLiteralFieldsWithRedaction(&bob, findCommand, opts.replacementForLiteralArgs.get());
    } else {
        addLiteralFieldsWithoutRedaction(&bob, findCommand);
    }

    // Add the fields that require no redaction.
    addRemainingFindCommandFields(&bob, findCommand);


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
        auto requestedSize = memory_util::convertToSizeInBytes(memSize);
        auto cappedSize = capTelemetryStoreSize(requestedSize);
        auto& telemetryStoreManager = telemetryStoreDecoration(serviceCtx);
        auto&& telemetryStore = telemetryStoreManager->getTelemetryStore();
        size_t numEvicted = telemetryStore.reset(cappedSize);
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
        // TODO SERVER-73907. Move this to run after FCV is initialized. It could be we'd have to
        // re-run this function if FCV changes later during the life of the process.
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
    // Cannot collect telemetry if sampling rate is not greater than 0. Note that we do not
    // increment telemetryRateLimitedRequestsMetric here since telemetry is entirely disabled.
    if (telemetryRateLimiter(serviceCtx)->getSamplingRate() <= 0) {
        return false;
    }
    // Check if rate limiting allows us to collect telemetry for this request.
    if (!telemetryRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        telemetryRateLimitedRequestsMetric.increment();
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
 * Upon reading telemetry data, we redact some keys. This is the list. See
 * TelemetryMetrics::redactKey().
 */
const stdx::unordered_set<std::string> kKeysToRedact = {"pipeline", "find"};

std::string sha256StringDataHasher(const StringData& fieldName) {
    auto hash = SHA256Block::computeHash({ConstDataRange(fieldName.rawData(), fieldName.size())});
    return hash.toString().substr(0, 12);
}

std::string sha256FieldNameHasher(const BSONElement& e) {
    auto&& fieldName = e.fieldNameStringData();
    return sha256StringDataHasher(fieldName);
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
}

static const StringData replacementForLiteralArgs = "?"_sd;

}  // namespace

StatusWith<BSONObj> TelemetryMetrics::redactKey(const BSONObj& key,
                                                bool redactIdentifiers,
                                                OperationContext* opCtx) const {
    // The redacted key for each entry is cached on first computation. However, if the redaction
    // straegy has flipped (from no redaction to SHA256, vice versa), we just return the key passed
    // to the function, so entries returned to the user match the redaction strategy requested in
    // the most recent telemetry command.
    if (!redactIdentifiers) {
        return key;
    }
    if (_redactedKey) {
        return *_redactedKey;
    }

    if (cmdObj.hasField(FindCommandRequest::kCommandName)) {
        tassert(7198600, "Find command must have a namespace string.", this->nss.nss().has_value());
        auto findCommand =
            query_request_helper::makeFromFindCommand(cmdObj, this->nss.nss().value(), false);

        SerializationOptions options(sha256StringDataHasher, replacementForLiteralArgs);
        auto nss = findCommand->getNamespaceOrUUID().nss();
        uassert(7349400, "Namespace must be defined", nss.has_value());
        auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, nss.value());
        expCtx->variables.setDefaultRuntimeConstants(opCtx);
        expCtx->maxFeatureCompatibilityVersion = boost::none;  // Ensure all features are allowed.
        expCtx->stopExpressionCounters();
        auto swRedactedKey = makeTelemetryKey(*findCommand, options, expCtx, *this);
        if (!swRedactedKey.isOK()) {
            return swRedactedKey.getStatus();
        }
        _redactedKey = std::move(swRedactedKey.getValue());
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

/**
 * Top-level checks for whether telemetry collection is enabled. If this returns false, we must go
 * no further.
 */
bool isTelemetryEnabled() {
    // During initialization FCV may not yet be setup but queries could be run. We can't
    // check whether telemetry should be enabled without FCV, so default to not recording
    // those queries.
    return serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        feature_flags::gFeatureFlagTelemetry.isEnabled(serverGlobalParams.featureCompatibility) &&
        getTelemetryStoreSize() != 0;
}

// The originating command/query does not persist through the end of query execution. In order to
// pair the telemetry metrics that are collected at the end of execution with the original query, it
// is necessary to register the original query during planning and persist it after
// execution.

// During planning, registerAggRequest or registerFindRequest are called to serialize the query
// shape and context (together, the telemetry context) and save it to OpDebug. Moreover, as query
// execution may span more than one request/operation and OpDebug does not persist through cursor
// iteration, it is necessary to communicate the telemetry context across operations. In this way,
// the telemetry context is registered to the cursor, so upon getMore() calls, the cursor manager
// passes the telemetry key from the pinned cursor to the new OpDebug.

// Once query execution is complete, the telemetry context is grabbed from OpDebug, a telemetry key
// is generated from this and metrics are paired to this key in the telemetry store.
void registerAggRequest(const AggregateCommandRequest& request, OperationContext* opCtx) {
    if (!isTelemetryEnabled()) {
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
    } catch (ExceptionFor<ErrorCodes::EncounteredFLEPayloadWhileRedacting>&) {
        return;
    }

    CurOp::get(opCtx)->debug().telemetryStoreKey = telemetryKey.obj();
}

void registerFindRequest(const FindCommandRequest& request,
                         const NamespaceString& collection,
                         OperationContext* opCtx,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!isTelemetryEnabled()) {
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
    options.replacementForLiteralArgs = replacementForLiteralArgs;
    auto swTelemetryKey = makeTelemetryKey(request, options, expCtx);
    tassert(7349402,
            str::stream() << "Error encountered when extracting query shape from command for "
                             "telemetry collection: "
                          << swTelemetryKey.getStatus().toString(),
            swTelemetryKey.isOK());

    CurOp::get(opCtx)->debug().telemetryStoreKey = std::move(swTelemetryKey.getValue());
}

TelemetryStore& getTelemetryStore(OperationContext* opCtx) {
    uassert(6579000,
            "Telemetry is not enabled without the feature flag on and a cache size greater than 0 "
            "bytes",
            isTelemetryEnabled());
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
        // This can happen if the budget is immediately exceeded. Specifically if the there is
        // not enough room for a single new entry if the number of partitions is too high
        // relative to the size.
        tassert(7064700, "Should find telemetry store entry", newMetrics.isOK());
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
