// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/analyze_cmd.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/analyze_command_gen.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_sketch_gen.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/stats_catalog.h"
#include "mongo/db/query/compiler/stats/stats_for_histograms_gen.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/version_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>
namespace mongo {
namespace {

// Total docs persisted across all analyze runs.
auto& analyzeDocsPersisted = *MetricBuilder<Counter64>{"query.analyze.sample.docsPersisted"};

// Sampling technique breakdown for how the sample was generated.
auto& analyzeByMethodRandom = *MetricBuilder<Counter64>{"query.analyze.sample.byMethod.random"};
auto& analyzeByMethodChunk = *MetricBuilder<Counter64>{"query.analyze.sample.byMethod.chunk"};
auto& analyzeByMethodFullCollScan =
    *MetricBuilder<Counter64>{"query.analyze.sample.byMethod.fullCollScan"};

// Latency of analyze's sample-mode path.
auto& analyzeMicros = *MetricBuilder<DurationCounter64<Microseconds>>{"commands.analyze.micros"};
// Histogram produces a vector bounds from 0.256 ms to 268000 ms (268 s)
auto& analyzeMicrosHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"commands.analyze.histograms.micros"}.bind(
        HistogramServerStatusMetric::pow(11, 256, 4));

// Histogram of pages persisted per analyze run. Bounds are 1 to 32 pages.
auto& analyzePagesHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.analyze.sample.histograms.pages"}.bind(
        HistogramServerStatusMetric::pow(6, 1, 2));

// Bytes persisted per analyze run, summed across all pages. Total and per-run. Bounds are 256
// bytes to ~268 MB.
auto& analyzeTotalBytesPersisted =
    *MetricBuilder<Counter64>{"query.analyze.sample.totalBytesPersisted"};
auto& analyzeBytesPersistedHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.analyze.sample.histograms.bytesPersisted"}
         .bind(HistogramServerStatusMetric::pow(11, 256, 4));


/**
 * Validates a user-provided key path. Shared by the histograms and ndv modes.
 */
void validateKeyPath(std::string_view key) {
    const FieldRef keyFieldRef(key);

    // Empty path
    uassert(6799703, "Key path is empty", !keyFieldRef.empty());

    for (size_t i = 0; i < keyFieldRef.numParts(); ++i) {
        uassertStatusOK(FieldPath::validateFieldName(keyFieldRef.getPart(i)));
    }

    // Numerics
    const auto numericPathComponents = keyFieldRef.getNumericPathComponents(0);
    uassert(6799704,
            str::stream() << "Key path contains numeric component "
                          << keyFieldRef.getPart(*(numericPathComponents.begin())),
            numericPathComponents.empty());
}

/**
 * Normalizes and validates the ndv-mode 'key' argument: the string form becomes a one-path
 * vector, the array form carries up to kNdvMaxFields distinct paths for one composite NDV
 * statistic. Returns the paths canonically sorted, the order the statistic is keyed and
 * persisted in (NDV is insensitive to field order).
 */
std::vector<std::string> normalizeNdvKeyPaths(
    const std::variant<std::string, std::vector<std::string>>& key) {
    std::vector<std::string> paths =
        visit(OverloadedVisitor{
                  [](const std::string& single) { return std::vector<std::string>{single}; },
                  [](const std::vector<std::string>& multiple) {
                      return multiple;
                  }},
              key);
    uassert(13176201,
            str::stream() << "ndv mode supports between 1 and " << ce::kNdvMaxFields
                          << " key paths",
            !paths.empty() && paths.size() <= ce::kNdvMaxFields);
    for (const auto& path : paths) {
        validateKeyPath(path);
    }
    std::sort(paths.begin(), paths.end());
    uassert(13176202,
            "ndv mode key paths must be distinct",
            std::adjacent_find(paths.begin(), paths.end()) == paths.end());
    // A path that prefixes another would collide in the inclusion $project of the ndv pipeline;
    // NDV over such a pair is degenerate anyway, so reject it with a clear message. A path
    // prefix is also a string prefix, so after the sort it can only precede its extensions.
    for (size_t i = 0; i + 1 < paths.size(); ++i) {
        for (size_t j = i + 1; j < paths.size(); ++j) {
            uassert(13176203,
                    str::stream() << "ndv mode key paths may not overlap: '" << paths[i]
                                  << "' is a prefix of '" << paths[j] << "'",
                    !FieldRef(paths[i]).isPrefixOf(FieldRef(paths[j])));
        }
    }
    return paths;
}

StatusWith<BSONObj> analyzeCommandAsAggregationCommand(OperationContext* opCtx,
                                                       std::string_view collection,
                                                       std::string_view keyPath,
                                                       boost::optional<double> sampleRate,
                                                       boost::optional<int> numBuckets) {
    // Build a pipeline that accomplishes the analyze request. The building code constructs a
    // pipeline that looks like this, assuming the analyze is on the key "a.b.c"
    //
    //      [
    //          { $match: { $expr: {$lt: [{$rand: {}, sampleRate]} } }, // If sampleRate is
    //          specified, otherwise this stage is omitted
    //          { $project: { val : "$a.b.c" } },
    //          { $group: {
    //              _id: "a.b.c",
    //              statistics: { $_internalConstructStats: {
    //                              val: "$$ROOT",
    //                              sampleRate: sampleRate,
    //                              numberBuckets: numberBuckets }
    //              }
    //          },
    //          { $merge: {
    //              into: "system.statistics." + collection,
    //              on: "key",
    //              whenMatched: "replace",
    //              whenNotMatched: "insert" }
    //          }
    //      ]
    //
    std::string into(str::stream() << NamespaceString::kStatisticsCollectionPrefix << collection);
    FieldPath fieldPath(keyPath);

    BSONArrayBuilder pipelineBuilder;

    if (sampleRate) {
        pipelineBuilder << BSON(
            "$match" << BSON(
                "$expr" << BSON("$lt" << BSON_ARRAY(BSON("$rand" << BSONObj()) << *sampleRate))));
    }

    InternalConstructStatsAccumulatorParams statsAccumParams;
    statsAccumParams.setVal("$$ROOT");
    statsAccumParams.setSampleRate(sampleRate ? *sampleRate : 1.0);
    statsAccumParams.setNumberBuckets(numBuckets ? *numBuckets
                                                 : mongo::stats::ScalarHistogram::kMaxBuckets);

    pipelineBuilder << BSON("$project" << BSON("val" << fieldPath.fullPathWithPrefix()))
                    << BSON("$group" << BSON("_id" << keyPath << "statistics"
                                                   << BSON("$_internalConstructStats"
                                                           << statsAccumParams.toBSON())))
                    << BSON("$merge" << BSON("into" << std::move(into) << "on"
                                                    << "_id"
                                                    << "whenMatched"
                                                    << "replace"
                                                    << "whenNotMatched"
                                                    << "insert"));

    return BSON("aggregate" << collection << "pipeline" << pipelineBuilder.arr() << "cursor"
                            << BSONObj() << "allowDiskUse" << false);
}

/**
 * Builds the ndv-mode aggregation. For the sorted key paths ["a.b", "c"] the pipeline looks
 * like this:
 *
 *      [
 *          { $project: { _id: 0, "a.b": 1, "c": 1 } },
 *          { $group: {
 *              _id: "1|<collection uuid>|a.b|c",
 *              sketches: { $_internalConstructNdvSketch: { val: "$$ROOT",
 *                                                          fields: ["a.b", "c"] } }
 *          } },
 *          { $project: {
 *              schemaVersion: {$literal: 1},
 *              collectionUuid: <UUID>,
 *              sortedFieldPaths: {$literal: ["a.b", "c"]},
 *              createdAt: "$$NOW",
 *              ndv: { sketches: "$sketches" }
 *          } },
 *          { $merge: { into: "system.stats.field_stats", on: "_id",
 *                      whenMatched: "replace", whenNotMatched: "insert" } }
 *      ]
 */
BSONObj analyzeNdvModeAsAggregationCommand(std::string_view collection,
                                           const std::vector<std::string>& keyPaths,
                                           const UUID& collUuid,
                                           const std::string& docId) {
    BSONArrayBuilder pipelineBuilder;
    {
        // Narrow the stream to the analyzed fields. An inclusion projection preserves document
        // structure and missing-ness, both of which the accumulator relies on. _id can only be
        // excluded when it is not among the analyzed fields itself.
        BSONObjBuilder stageBob(pipelineBuilder.subobjStart());
        BSONObjBuilder projectBob(stageBob.subobjStart("$project"));
        if (std::none_of(keyPaths.begin(), keyPaths.end(), [](const std::string& path) {
                return FieldRef(path).getPart(0) == "_id";
            })) {
            projectBob.append("_id", 0);
        }
        for (const auto& path : keyPaths) {
            projectBob.append(path, 1);
        }
    }
    {
        // The sketch-building $group.
        InternalConstructNdvSketchAccumulatorParams sketchParams;
        sketchParams.setVal("$$ROOT");
        sketchParams.setFields(keyPaths);

        BSONObjBuilder stageBob(pipelineBuilder.subobjStart());
        BSONObjBuilder groupBob(stageBob.subobjStart("$group"));
        // The id string always starts with the schema version digit, never '$', so it parses
        // as a plain string constant.
        groupBob.append("_id", docId);
        groupBob.append("sketches", BSON("$_internalConstructNdvSketch" << sketchParams.toBSON()));
    }
    {
        // The stamping $project. All values are constants except the sketches reference; the
        // BinData UUID and $literal-wrapped values cannot be mistaken for inclusion flags or
        // paths.
        BSONObjBuilder stageBob(pipelineBuilder.subobjStart());
        BSONObjBuilder projectBob(stageBob.subobjStart("$project"));
        projectBob.append("schemaVersion", BSON("$literal" << ce::kFieldStatsSchemaVersion));
        collUuid.appendToBuilder(&projectBob, "collectionUuid");
        {
            BSONArrayBuilder pathsBob;
            for (const auto& path : keyPaths) {
                pathsBob.append(path);
            }
            projectBob.append("sortedFieldPaths", BSON("$literal" << pathsBob.arr()));
        }
        projectBob.append("createdAt", "$$NOW");
        projectBob.append("ndv", BSON("sketches" << "$sketches"));
    }
    {
        // 'replace' is only safe while ndv is the sole statistic section in the document; once
        // a second one exists this must become a $set-style pipeline update so sections written
        // by other analyze runs survive.
        BSONObjBuilder stageBob(pipelineBuilder.subobjStart());
        BSONObjBuilder mergeBob(stageBob.subobjStart("$merge"));
        mergeBob.append("into", NamespaceString::kStatsFieldStatsCollectionName);
        mergeBob.append("on", "_id");
        mergeBob.append("whenMatched", "replace");
        mergeBob.append("whenNotMatched", "insert");
    }

    BSONObjBuilder aggBob;
    aggBob.append("aggregate", collection);
    aggBob.appendArray("pipeline", pipelineBuilder.arr());
    // The acquisition used for validation is released by the time this command runs; the UUID
    // check makes the aggregation fail instead of persisting stats for a dropped-and-recreated
    // collection under the old UUID.
    collUuid.appendToBuilder(&aggBob, "collectionUUID");
    aggBob.append("cursor", BSONObj());
    aggBob.append("allowDiskUse", false);
    // TODO SERVER-132681: Consider using a covered IXSCAN instead of the collection scan.
    aggBob.append("hint", BSON("$natural" << 1));
    return aggBob.obj();
}

/**
 * Acquires 'nss' for reading and performs the collection validation shared by the sample and
 * ndv modes: the collection must exist ('notFoundCode' preserves each mode's error code) and
 * must not be timeseries. Views never reach this point; the acquisition rejects them.
 */
CollectionAcquisition acquireAndValidateCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   int notFoundCode) {
    auto coll = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));

    uassert(notFoundCode,
            str::stream() << "Couldn't find collection " << nss.toStringForErrorMsg(),
            coll.exists());

    // A scan of a timeseries collection sees the internal bucket documents rather than the
    // user's fields, so statistics would be measured on the wrong thing.
    uassert(ErrorCodes::CommandNotSupported,
            "Analyze command is not supported on timeseries collections",
            !coll.getCollectionPtr()->isTimeseriesCollection() ||
                !coll.getCollectionPtr()->isNewTimeseriesWithoutView());

    return coll;
}

void runNdvMode(OperationContext* opCtx,
                const NamespaceString& nss,
                const std::variant<std::string, std::vector<std::string>>& key) {
    uassert(ErrorCodes::CommandNotSupported,
            "The analyze command with ndv mode requires featureFlagPersistentStats to be enabled",
            feature_flags::gFeatureFlagPersistentStats.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    // The knob gates the whole persistent-NDV feature: while it is off, statistics are neither
    // collected nor consumed.
    uassert(ErrorCodes::CommandNotSupported,
            "The analyze command with ndv mode requires internalQueryEnablePersistentNDVStats to "
            "be enabled",
            QueryKnobConfiguration(query_settings::QuerySettings{}).getEnablePersistentNDVStats());

    // Validated only after the feature gates above: without them the command must fail with
    // CommandNotSupported, not with key validation errors for a disabled feature.
    const std::vector<std::string> keyPaths = normalizeNdvKeyPaths(key);

    boost::optional<UUID> collUuid;
    {
        // Shared validation deliberately mirrors sample mode, not histograms mode: capped
        // collections are fine to read for NDV.
        const auto coll = acquireAndValidateCollection(opCtx, nss, 13175802);

        // Same restriction as histograms mode: no NDV statistics on system collections.
        uassert(13175804,
                str::stream() << nss.toStringForErrorMsg()
                              << " is not a normal or clustered collection",
                nss.isNormalCollection() || coll.getCollectionPtr()->isClustered());

        // system.stats.field_stats is shard-local and invisible through mongos.
        // TODO SERVER-133119: Support NDV statistics for sharded collections.
        uassert(13175803,
                "ndv mode is not supported on sharded collections",
                !coll.getShardingDescription().isSharded());

        collUuid = coll.getCollectionPtr()->uuid();
    }

    // The pipeline uses an internal-only accumulator and merges into a system collection, both
    // of which require internal permissions.
    const bool wasInternalClient = isInternalClient(opCtx->getClient());
    if (!wasInternalClient) {
        opCtx->getClient()->setIsInternalClient(true);
    }
    ScopeGuard resetInternalClient([&] {
        if (!wasInternalClient) {
            opCtx->getClient()->setIsInternalClient(false);
        }
    });

    DBDirectClient client(opCtx);
    const std::string docId = ce::makeFieldStatsId(*collUuid, keyPaths);

    // Note: an empty collection produces no $group output, so a previous stats document is left
    // in place. Stats invalidation (also after emptying or dropping a collection) is out of
    // scope for now; the read path guards against staleness instead.
    BSONObj result;
    client.runCommand(nss.dbName(),
                      analyzeNdvModeAsAggregationCommand(nss.coll(), keyPaths, *collUuid, docId),
                      result);
    uassertStatusOK(getStatusFromCommandResult(result));
}

void runSampleMode(OperationContext* opCtx,
                   const NamespaceString& nss,
                   boost::optional<int> sampleSizeOpt,
                   boost::optional<SamplingCEMethodEnum> requestedSamplingMethodOpt,
                   boost::optional<int> numChunksOpt) {
    auto* tickSource = opCtx->getServiceContext()->getTickSource();
    auto startTicks = tickSource->getTicks();

    uassert(ErrorCodes::CommandNotSupported,
            "The analyze command with sampling mode requires featureFlagPersistentStats to be "
            "enabled",
            feature_flags::gFeatureFlagPersistentStats.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    // 'samplingMethod' is optional on the command. Default to the
    // persistent-sample read path's method (internalQuerySamplingCEMethodForPersistentSamples).
    QueryKnobConfiguration qkc(query_settings::QuerySettings{});
    const SamplingCEMethodEnum requestedSamplingMethod = requestedSamplingMethodOpt.value_or(
        qkc.getInternalQuerySamplingCEMethodForPersistentSamples());

    boost::optional<ce::SamplingTechniqueEnum> actualSamplingMethod;
    boost::optional<UUID> collUUID;
    std::vector<BSONObj> docsArr;
    size_t sampleSize;
    // Tracks the actual number of docs persisted for server status metric on successful upsert.
    size_t docsPersistedCount = 0;

    {
        // Acquire the collection to read metadata and run the sampling estimator. The acquisition
        // must remain live for the duration of sampling.
        auto coll = acquireAndValidateCollection(opCtx, nss, 12433000);
        const auto& collectionPtr = coll.getCollectionPtr();

        collUUID = collectionPtr->uuid();
        long long numRecords = collectionPtr->numRecords(opCtx);

        if (sampleSizeOpt) {
            sampleSize = *sampleSizeOpt;
        } else {
            sampleSize = ce::SamplingEstimatorImpl::calculateSampleSize(qkc);
        }

        if (requestedSamplingMethod == SamplingCEMethodEnum::kChunk && !numChunksOpt) {
            numChunksOpt = internalQueryNumChunksForChunkBasedSampling.load();
        }

        tassert(12433003,
                "numChunks must be set for chunk-based sampling",
                !(requestedSamplingMethod == SamplingCEMethodEnum::kChunk && !numChunksOpt));

        MultipleCollectionAccessor collections(coll);

        // Use kOnTheFlySample to force collection of a new sample rather than attempting to
        // read an existing one.
        // TODO SERVER-127210: Investigate if this is the right yield policy to ensure we sample
        // from a consistent snapshot.
        ce::SamplingEstimatorImpl estimator(opCtx,
                                            collections,
                                            nss,
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            sampleSize,
                                            requestedSamplingMethod,
                                            numChunksOpt,
                                            numRecords,
                                            nullptr /*customerQueryExpCtx*/,
                                            SamplingSourceEnum::kOnTheFlySample);
        estimator.generateSample(ce::NoProjection{});

        // Copy the sample so it outlives the estimator and can be persisted.
        docsArr = estimator.getSample();

        // Store the sampling method that was actually used (which may differ from
        // requestedSamplingMethod when test-only knobs like
        // internalQuerySamplingBySequentialScan are enabled).
        actualSamplingMethod = estimator.getSamplingMetadata().technique;
    }

    tassert(12433002, "collUUID must be initialized by end of sampling block", collUUID);
    tassert(12873101,
            "actualSamplingMethod must be initialized by end of sampling block",
            actualSamplingMethod);

    // A full collection scan is performed whenever sample size is >= collection size regardless
    // of requested sampling method, so the value persisted in the sample doc should still
    // reflect the requested method in this case. Otherwise it should reflect the actual method
    // used.
    ce::SamplingTechniqueEnum samplingMethodToPersist =
        *actualSamplingMethod == ce::SamplingTechniqueEnum::kFullCollScan
        ? ce::SamplingEstimatorImpl::samplingMethodToTechnique(requestedSamplingMethod)
        : *actualSamplingMethod;

    if (samplingMethodToPersist != ce::SamplingTechniqueEnum::kChunk) {
        numChunksOpt = boost::none;
    }

    const Date_t createdAt = Date_t::now();

    // Create a clustered collection for persistent sample.
    const NamespaceString samplesNss = NamespaceStringUtil::deserialize(
        nss.dbName(), NamespaceString::kStatsSamplesCollectionName);
    auto createCollectionStatus = repl::StorageInterface::get(opCtx)->createCollection(
        opCtx,
        samplesNss,
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()});
    // Samples collection may already exist, in which case the createCollection command
    // was a no-op.
    if (createCollectionStatus != ErrorCodes::NamespaceExists) {
        uassertStatusOK(createCollectionStatus);
    }

    // Serialize the sample into one or more page documents
    std::vector<BSONObj> pageDocs = ce::makePersistentSamplePageDocs(
        *collUUID, samplingMethodToPersist, sampleSize, numChunksOpt, docsArr, createdAt);
    const size_t pagesPersisted = pageDocs.size();
    size_t totalBytesPersisted = 0;
    std::vector<InsertStatement> inserts;
    inserts.reserve(pageDocs.size());
    for (auto& pageDoc : pageDocs) {
        totalBytesPersisted += pageDoc.objsize();
        const BSONElement docsField = pageDoc.getField(ce::PersistentSampleDoc::kDocsFieldName);
        tassert(13106002,
                str::stream() << "Expected " << ce::PersistentSampleDoc::kDocsFieldName
                              << " to be an array, but found " << typeName(docsField.type()),
                docsField.type() == BSONType::array);
        // kDocsFieldName is an array and nFields return its size
        docsPersistedCount += static_cast<size_t>(docsField.Obj().nFields());
        inserts.emplace_back(std::move(pageDoc));
    }

    const BSONObj existingSampleLookupFilter = ce::makePersistentSampleAllPagesLookupFilter(
        *collUUID, samplingMethodToPersist, sampleSize, numChunksOpt);

    // Atomically insert the sample docs/replace the existing sample if one exists.
    writeConflictRetry(opCtx, "analyzePersistSample", samplesNss, [&] {
        const auto collection =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, samplesNss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);
        uassert(12844400,
                str::stream() << "Sample collection " << samplesNss.toStringForErrorMsg()
                              << " does not exist",
                collection.exists());

        WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);

        // Remove all existing pages of any prior sample.
        deleteObjects(
            opCtx, collection, existingSampleLookupFilter, false /*justOne*/, true /*god*/);

        // Insert the new sample.
        uassertStatusOK(collection_internal::insertDocuments(opCtx,
                                                             collection.getCollectionPtr(),
                                                             inserts.begin(),
                                                             inserts.end(),
                                                             nullptr /*opDebug*/));

        wuow.commit();
    });

    // Increment metrics
    auto durationMicros = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTicks);
    analyzeMicros.increment(durationMicros);
    analyzeMicrosHistogram.increment(durationCount<Microseconds>(durationMicros));

    analyzeDocsPersisted.incrementRelaxed(docsPersistedCount);
    switch (actualSamplingMethod.value()) {
        case ce::SamplingTechniqueEnum::kChunk:
            analyzeByMethodChunk.incrementRelaxed();
            break;
        case ce::SamplingTechniqueEnum::kFullCollScan:
            analyzeByMethodFullCollScan.incrementRelaxed();
            break;
        case ce::SamplingTechniqueEnum::kRandom:
            analyzeByMethodRandom.incrementRelaxed();
            break;
        case ce::SamplingTechniqueEnum::kSeqScan:
        case ce::SamplingTechniqueEnum::kStrides:
            // Since kSeqScan and kStrides are test-only sampling techniques we do
            // not keep track/update in server metrics since count will always be 0.
            break;
    }

    analyzePagesHistogram.increment(pagesPersisted);
    analyzeTotalBytesPersisted.incrementRelaxed(totalBytesPersisted);
    analyzeBytesPersistedHistogram.increment(totalBytesPersisted);
}

void runHistogramsMode(OperationContext* opCtx,
                       const NamespaceString& nss,
                       bool explicitHistogramsMode,
                       boost::optional<std::string_view> key,
                       boost::optional<double> sampleRate,
                       boost::optional<int> sampleSize,
                       boost::optional<int> numberBuckets) {
    uassert(ErrorCodes::CommandNotSupported, "no such command: analyze", getTestCommandsEnabled());

    // Without an explicit mode this is the legacy default flow, where a missing key merely
    // validates the collection.
    if (explicitHistogramsMode) {
        uassert(9820001, "Histograms mode requires a key to be specified", key);
    }

    // Sample rate and sample size can't both be present
    uassert(6799705,
            "Only one of sampleRate and sampleSize may be present",
            !sampleRate || !sampleSize);

    // Validate collection
    {
        const auto coll = acquireAndValidateCollection(opCtx, nss, 6799700);
        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::ReadLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(nss.dbName()));
        const auto& collectionPtr = coll.getCollectionPtr();

        // Namespace cannot be capped collection
        uassert(6799701,
                str::stream() << "Analyze command is not supported on capped collections",
                !collectionPtr->isCapped());

        // Namespace is normal or clustered collection
        uassert(6799702,
                str::stream() << nss.toStringForErrorMsg()
                              << " is not a normal or clustered collection",
                nss.isNormalCollection() || collectionPtr->isClustered());

        if (sampleSize) {
            const auto numRecords = collectionPtr->numRecords(opCtx);
            if (numRecords == 0 || *sampleSize > numRecords) {
                sampleRate = 1.0;
            } else {
                sampleRate = double(*sampleSize) / numRecords;
            }
        }
    }

    // Validate key
    if (key) {
        validateKeyPath(*key);

        // We need to perform this operation with internal permissions.
        const bool wasInternalClient = isInternalClient(opCtx->getClient());
        if (!wasInternalClient) {
            opCtx->getClient()->setIsInternalClient(true);
        }

        DBDirectClient client(opCtx);

        // Run Aggregate
        BSONObj analyzeResult;
        client.runCommand(nss.dbName(),
                          analyzeCommandAsAggregationCommand(
                              opCtx, nss.coll(), std::string{*key}, sampleRate, numberBuckets)
                              .getValue(),
                          analyzeResult);

        // We must reset the internal flag.
        if (!wasInternalClient) {
            opCtx->getClient()->setIsInternalClient(false);
        }

        uassertStatusOK(getStatusFromCommandResult(analyzeResult));

        // Invalidate statistics in the cache for the analyzed path
        stats::StatsCatalog& statsCatalog = stats::StatsCatalog::get(opCtx);
        uassertStatusOK(statsCatalog.invalidatePath(nss, std::string{*key}));
    } else if (sampleSize || sampleRate) {
        uassert(6799706,
                "It is illegal to pass sampleRate or sampleSize without a key in "
                "histograms mode",
                key);
    }
}

class CmdAnalyze final : public TypedCommand<CmdAnalyze> {
public:
    using Request = AnalyzeCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to generate statistics for a collection for use in the optimizer.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void typedRun(OperationContext* opCtx) {
            const auto& cmd = request();
            const NamespaceString& nss = ns();

            // TODO SERVER-127476: Make sample mode the default
            // TODO SERVER-133120: Model the analyze command as an abstract class with one
            // implementation per mode instead of dispatching here.
            const auto mode = cmd.getMode();
            const auto& key = cmd.getKey();
            // Only ndv mode gives the array form of 'key' a meaning.
            uassert(13176200,
                    "an array of key paths is only supported with mode: \"ndv\"",
                    !key || std::holds_alternative<std::string>(*key) ||
                        (mode && *mode == AnalyzeModeEnum::kNdv));
            if (mode && *mode == AnalyzeModeEnum::kSample) {
                runSampleMode(
                    opCtx, nss, cmd.getSampleSize(), cmd.getSamplingMethod(), cmd.getNumChunks());
            } else if (mode && *mode == AnalyzeModeEnum::kNdv) {
                uassert(13175800, "ndv mode requires a key to be specified", key);
                uassert(13175801,
                        "sampleRate, sampleSize, numberBuckets, samplingMethod and numChunks "
                        "are not supported with ndv mode",
                        !cmd.getSampleRate() && !cmd.getSampleSize() && !cmd.getNumberBuckets() &&
                            !cmd.getSamplingMethod() && !cmd.getNumChunks());
                runNdvMode(opCtx, nss, *key);
            } else {
                runHistogramsMode(
                    opCtx,
                    nss,
                    mode.has_value() /* explicitHistogramsMode */,
                    key ? boost::optional<std::string_view>(std::get<std::string>(*key))
                        : boost::none,
                    cmd.getSampleRate(),
                    cmd.getSampleSize(),
                    cmd.getNumberBuckets());
            }
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            const NamespaceString& ns = request().getNamespace();

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call analyze on collection "
                                  << ns.toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnNamespace(ns, ActionType::analyze));

            // Require find privilege to prevent analyze from being used as a proxy to read
            // documents from collections the caller cannot directly access.
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to read collection "
                                  << ns.toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnNamespace(ns, ActionType::find));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdAnalyze).forShard();

}  // namespace
}  // namespace mongo
