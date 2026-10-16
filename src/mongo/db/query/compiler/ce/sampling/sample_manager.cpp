// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/sample_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/on_the_fly_sample_loader.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {

namespace {

/**
 * Maximum overhead a single object element could add to an open BSON array:
 *  - 1 byte for the element type
 *  - 1 byte for the field name's NUL terminator
 *  - 1 byte per digit of the max possible array index.
 */
const int kArrayElementMaxOverheadBytes =
    2 + static_cast<int>(std::to_string(BSONObjMaxUserSize / BSONObj::kMinBSONLength).length());

/**
 * Trailing bytes required close a page's open `docs` array and its enclosing object.
 * 1 EOO byte per open scope.
 */
constexpr int kBytesToClosePage = 2;

/**
 * Number of bytes taken up by the `numDocsDiscarded` field in the PersistentSampleDoc type.
 */
constexpr int kNumDocsDiscardedFieldBytes = 1 /* type indicator */ +
    (sizeof("numDocsDiscarded")) /* field name (including null-terminator) */
    + 8 /* long type */;

/**
 * Appends every field of a sample page document except the `docs` array, leaving `builder` ready
 * for the caller to append that array.
 */
void appendSamplePageMetadata(BSONObjBuilder& builder,
                              const UUID& collectionUuid,
                              SamplingTechniqueEnum method,
                              size_t requestedSampleSize,
                              boost::optional<int> numChunks,
                              int pageNo,
                              Date_t createdAt) {
    const std::string id =
        makePersistentSampleId(collectionUuid, method, requestedSampleSize, numChunks, pageNo);

    builder.append(PersistentSampleDoc::k_idFieldName, id);
    builder.append(PersistentSampleDoc::kPageNoFieldName, pageNo);
    builder.append(PersistentSampleDoc::kCollectionUuidFieldName, collectionUuid.toString());
    builder.append(PersistentSampleDoc::kSchemaVersionFieldName, kPersistentSampleSchemaVersion);
    builder.appendDate(PersistentSampleDoc::kCreatedAtFieldName, createdAt);
    builder.append(PersistentSampleDoc::kRequestedSampleSizeFieldName,
                   static_cast<long long>(requestedSampleSize));
    builder.append(PersistentSampleDoc::kSamplingMethodFieldName, idlSerialize(method));
    if (method == SamplingTechniqueEnum::kChunk) {
        builder.append(PersistentSampleDoc::kNumChunksFieldName, *numChunks);
    }
}

}  // namespace

std::vector<BSONObj> makePersistentSamplePageDocs(const UUID& collectionUuid,
                                                  SamplingTechniqueEnum method,
                                                  size_t requestedSampleSize,
                                                  boost::optional<int> numChunks,
                                                  const std::vector<BSONObj>& sample,
                                                  Date_t createdAt) {
    // The `numDocsDiscarded` field needs to be the same across all pages and we can't determine it
    // until after we've looped through the entire sample. So we store all the builders in a vector
    // so that we can keep them open until we're ready to append `numDocsDiscarded` to the page.
    // Then we can close them.
    std::vector<BSONObjBuilder> builders;
    size_t docIdx = 0;
    int pageNo = 0;
    size_t numDiscarded = 0;

    const double maxDiscardFraction = internalQueryMaxPersistentSampleDiscardFraction.load();
    do {
        builders.emplace_back();
        auto& builder = builders.back();
        appendSamplePageMetadata(
            builder, collectionUuid, method, requestedSampleSize, numChunks, pageNo, createdAt);
        BSONArrayBuilder docsArr(builder.subarrayStart(PersistentSampleDoc::kDocsFieldName));

        // Size of this page with its metadata but no documents.
        const int emptyPageLenBytes = builder.len();

        int docsOnPage = 0;
        while (docIdx < sample.size()) {
            const BSONObj& doc = sample[docIdx];

            // Calculate the maximum number of bytes this document would add to a page.
            const int docSizeWithOverheadBytes = doc.objsize() + kArrayElementMaxOverheadBytes +
                kBytesToClosePage + kNumDocsDiscardedFieldBytes;

            // Calculate the maximum size the page could be after adding this document.
            const int projectedPageSize = builder.len() + docSizeWithOverheadBytes;

            if (emptyPageLenBytes + docSizeWithOverheadBytes > BSONObjMaxUserSize) {
                // A document that cannot fit on an otherwise empty page can never be persisted.
                // Discard it and continue filling the current page, unless too much of the full
                // sample has been discarded already.
                ++numDiscarded;
                ++docIdx;
                LOGV2_WARNING(13106001,
                              "Discarding sampled document that exceeds the maximum persistable "
                              "size",
                              "collectionUuid"_attr = collectionUuid,
                              "docSizeBytes"_attr = doc.objsize(),
                              "numDiscarded"_attr = numDiscarded,
                              "sampleSize"_attr = sample.size());
                uassert(13106000,
                        str::stream() << "Sampled documents discarded due to exceeding the maximum "
                                         "persistable size ("
                                      << numDiscarded << " of " << sample.size()
                                      << ") exceed the maximum discardable fraction of the sample ("
                                      << maxDiscardFraction * 100 << "%)",
                        static_cast<double>(numDiscarded) <=
                            maxDiscardFraction * static_cast<double>(sample.size()));
                continue;
            } else if (projectedPageSize > BSONObjMaxUserSize) {
                // This doc doesn't fit on the current page but might fit on a fresh one. Leave
                // it for the next page.
                break;
            }
            docsArr.append(doc);
            ++docsOnPage;
            ++docIdx;
        }


        docsArr.done();

        // Every doc left for this page turned out to be too large to persist, so the page would be
        // empty. Don't persist an empty page unless it is the only page of an empty sample.
        if (docsOnPage == 0 && pageNo > 0) {
            builders.pop_back();
            break;
        }

        ++pageNo;
    } while (docIdx < sample.size());

    std::vector<BSONObj> pages;
    pages.reserve(builders.size());
    for (auto& builder : builders) {
        builder.append(PersistentSampleDoc::kNumDocsDiscardedFieldName,
                       static_cast<long long>(numDiscarded));
        BSONObj page = builder.obj();
        tassert(13044800,
                str::stream() << "Assembled sample page of " << page.objsize()
                              << " bytes exceeds the maximum BSON size of " << BSONObjMaxUserSize
                              << " bytes",
                page.objsize() <= BSONObjMaxUserSize);
        pages.push_back(std::move(page));
    }
    return pages;
}

SampleManager::SampleManager(OperationContext* opCtx,
                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                             boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx)
    : _opCtx(opCtx),
      _yieldPolicy(yieldPolicy),
      _customerQueryExpCtx(std::move(customerQueryExpCtx)) {}

Sample SampleManager::load(const MultipleCollectionAccessor& collections, const SampleSpec& spec) {
    // Try to load a persisted sample if one exists. If unsuccessful, load a new sample on the fly.
    if (spec.samplingSource == SamplingSourceEnum::kPersistentSample) {
        std::unique_ptr<PersistentSampleLoader> loader =
            makePersistentSampleLoader(_opCtx, collections, _yieldPolicy, _customerQueryExpCtx);
        auto loadStatus = loader->tryLoad(spec);
        if (loadStatus.isOK()) {
            return std::move(loadStatus.getValue());
        }

        if (loadStatus.getStatus().code() != ErrorCodes::NoSuchKey) {
            LOGV2_WARNING(12432806,
                          "Persistent sample not usable; falling back to on-the-fly sampling",
                          "nss"_attr = spec.nss.toStringForErrorMsg(),
                          "error"_attr = loadStatus.getStatus());
        }
    }

    SampleSpec onTheFlySpec = spec;
    onTheFlySpec.samplingSource = SamplingSourceEnum::kOnTheFlySample;
    std::unique_ptr<OnTheFlySampleLoader> loader = makeOnTheFlySampleLoader(
        _opCtx, collections, _yieldPolicy, _customerQueryExpCtx, onTheFlySpec);
    auto loadStatus = loader->tryLoad(onTheFlySpec);
    uassertStatusOK(loadStatus.getStatus());

    return std::move(loadStatus.getValue());
}

StatusWith<PersistedSampleSummary> SampleManager::refresh(const SampleSpec& spec) {
    tassert(13097203,
            "refresh() requires a spec that requests an on-the-fly sample",
            spec.samplingSource == SamplingSourceEnum::kOnTheFlySample);

    // Acquire the source collection to generate the sample.
    auto sourceCollection = acquireCollectionMaybeLockFree(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, spec.nss, AcquisitionPrerequisites::kRead));

    uassert(13097202,
            str::stream() << "Couldn't find collection " << spec.nss.toStringForErrorMsg(),
            sourceCollection.exists());

    uassert(ErrorCodes::IllegalOperation,
            "Persistent samples aren't supported on timeseries collections",
            !sourceCollection.getCollectionPtr()->isTimeseriesCollection() ||
                !sourceCollection.getCollectionPtr()->isNewTimeseriesWithoutView());

    const UUID collectionUuid = sourceCollection.getCollectionPtr()->uuid();
    MultipleCollectionAccessor collections(std::move(sourceCollection));
    Sample sample = load(collections, spec);
    // Release the source collection before persisting the sample since it is no longer needed.
    collections.clear();

    return persistAndReplaceExisting(spec, sample, collectionUuid, Date_t::now());
}

StatusWith<PersistedSampleSummary> SampleManager::persistAndReplaceExisting(
    const SampleSpec& spec, const Sample& sample, const UUID& collectionUuid, Date_t createdAt) {
    const SamplingTechniqueEnum actualSamplingMethod = sample.technique();
    // A full collection scan is performed whenever sample size is >= collection size regardless
    // of requested sampling method, so the value persisted in the sample doc should still
    // reflect the requested method in this case. Otherwise it should reflect the actual method
    // used.
    // TODO SERVER-129240
    const SamplingTechniqueEnum samplingMethodToPersist =
        actualSamplingMethod == SamplingTechniqueEnum::kFullCollScan
        ? samplingMethodToTechnique(spec.onTheFlySamplingMethod)
        : actualSamplingMethod;

    const boost::optional<int> numChunks =
        samplingMethodToPersist == SamplingTechniqueEnum::kChunk ? spec.numChunks : boost::none;

    // Create a clustered collection for persistent sample.
    const NamespaceString samplesNss =
        NamespaceStringUtil::deserialize(spec.nss.dbName(), kSamplesCollectionName);
    auto createCollectionStatus = repl::StorageInterface::get(_opCtx)->createCollection(
        _opCtx,
        samplesNss,
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()});
    // Samples collection may already exist, in which case the createCollection command
    // was a no-op.
    if (createCollectionStatus != ErrorCodes::NamespaceExists) {
        uassertStatusOK(createCollectionStatus);
    }

    // Serialize the sample into one or more page documents
    std::vector<BSONObj> pageDocs = makePersistentSamplePageDocs(collectionUuid,
                                                                 samplingMethodToPersist,
                                                                 spec.requestedSampleSize,
                                                                 numChunks,
                                                                 sample.docs(),
                                                                 createdAt);
    const size_t pagesPersisted = pageDocs.size();
    size_t totalBytesPersisted = 0;
    // Tracks the actual number of docs persisted for server status metric on successful upsert.
    size_t docsPersistedCount = 0;
    std::vector<InsertStatement> inserts;
    inserts.reserve(pageDocs.size());
    for (auto& pageDoc : pageDocs) {
        totalBytesPersisted += pageDoc.objsize();
        const BSONElement docsField = pageDoc.getField(PersistentSampleDoc::kDocsFieldName);
        tassert(13106002,
                str::stream() << "Expected " << PersistentSampleDoc::kDocsFieldName
                              << " to be an array, but found " << typeName(docsField.type()),
                docsField.type() == BSONType::array);
        // kDocsFieldName is an array and nFields return its size
        docsPersistedCount += static_cast<size_t>(docsField.Obj().nFields());
        inserts.emplace_back(std::move(pageDoc));
    }

    const auto [minId, maxId] = makePersistentSampleIdRange(
        collectionUuid, samplingMethodToPersist, spec.requestedSampleSize, numChunks);

    // Atomically insert the sample docs/replace the existing sample if one exists.
    writeConflictRetry(_opCtx, "analyzePersistSample", samplesNss, [&] {
        const auto collection =
            acquireCollection(_opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  _opCtx, samplesNss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);
        uassert(12844400,
                str::stream() << "Sample collection " << samplesNss.toStringForErrorMsg()
                              << " does not exist",
                collection.exists());

        tassert(13097201,
                "Samples collection must be clustered on _id",
                collection.getCollectionPtr()->isClustered());

        WriteUnitOfWork wuow(_opCtx, WriteUnitOfWork::atomicGroup);

        // Remove all existing pages of any prior sample.
        const auto recordIdForId = [](std::string_view id) {
            return RecordIdBound(
                record_id_helpers::keyForObj(BSON(PersistentSampleDoc::k_idFieldName << id)));
        };
        DeleteStageParams deleteParams;
        deleteParams.isMulti = true;
        auto deleteExec = InternalPlanner::deleteWithCollectionScan(
            _opCtx,
            collection,
            deleteParams,
            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
            InternalPlanner::FORWARD,
            recordIdForId(minId),
            recordIdForId(maxId),
            CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords);
        deleteExec->executeDelete();

        // Insert the new sample.
        uassertStatusOK(collection_internal::insertDocuments(_opCtx,
                                                             collection.getCollectionPtr(),
                                                             inserts.begin(),
                                                             inserts.end(),
                                                             /*opDebug=*/nullptr));

        wuow.commit();
    });

    return PersistedSampleSummary{.samplingMethod = actualSamplingMethod,
                                  .docsPersisted = docsPersistedCount,
                                  .pagesPersisted = pagesPersisted,
                                  .bytesPersisted = totalBytesPersisted};
}

}  // namespace mongo::ce
