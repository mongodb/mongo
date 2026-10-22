// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo::ce {

namespace {
/**
 * Separator between the parts of a persistent sample `_id`. Must sort strictly greater than every
 * digit so that a sample whose identity prefix is a numeric prefix of another's (e.g.
 * requestedSampleSize 1 vs 10) sorts outside the latter's page range rather than inside it.
 */
constexpr char kIdSeparator = '_';
static_assert(kIdSeparator > '9');

/**
 * Number of bytes taken up by the `numDocsDiscarded` field in the PersistentSampleDoc type.
 */
constexpr int kNumDocsDiscardedFieldBytes = 1 /* type indicator */ +
    (sizeof("numDocsDiscarded")) /* field name (including null-terminator) */
    + 8 /* long type */;

/**
 * Number of digits the page number is zero-padded to for a sample of the given size.
 */
size_t pageNoWidth(size_t requestedSampleSize) {
    return fmt::formatted_size("{}", requestedSampleSize);
}

/**
 * Builds the part of the `_id` shared by every page of a sample, up to and including the separator
 * that precedes the page number.
 */
std::string makePersistentSampleIdPrefix(const UUID& collectionUuid,
                                         SamplingTechniqueEnum method,
                                         size_t requestedSampleSize,
                                         boost::optional<int> numChunks) {
    tassert(12432800,
            "Chunk-based persistent sample ID requires numChunks",
            method != SamplingTechniqueEnum::kChunk || numChunks.has_value());
    tassert(12432801,
            "numChunks must only be set for chunk-technique persistent samples",
            method == SamplingTechniqueEnum::kChunk || !numChunks.has_value());
    tassert(12832700,
            "A persistent sample document should never be created or looked up with sampling "
            "method kFullCollScan",
            method != SamplingTechniqueEnum::kFullCollScan);
    tassert(13102000,
            "Persistent sample ID requires a positive requested sample size",
            requestedSampleSize > 0);

    std::string prefix = fmt::format("{}{}{}{}{}{}{}{}",
                                     collectionUuid.toString(),
                                     kIdSeparator,
                                     kPersistentSampleSchemaVersion,
                                     kIdSeparator,
                                     idlSerialize(method),
                                     kIdSeparator,
                                     requestedSampleSize,
                                     kIdSeparator);
    if (numChunks) {
        fmt::format_to(std::back_inserter(prefix), "{}{}", *numChunks, kIdSeparator);
    }
    return prefix;
}

/**
 * Scans the persistent samples collection for every page of the sample within the
 * provided range (inclusive)
 */
StatusWith<std::vector<BSONObj>> findPersistentSamplePages(OperationContext* opCtx,
                                                           const NamespaceString& samplesNss,
                                                           std::string_view minId,
                                                           std::string_view maxId) {
    std::vector<BSONObj> pages;
    try {
        const auto collection = acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest(samplesNss,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead));
        if (!collection.exists()) {
            return Status(ErrorCodes::NoSuchKey, "samples collection does not exist");
        }

        if (!collection.getCollectionPtr()->isClustered()) {
            // TODO SERVER-131864 consider if this should tassert instead
            return Status(ErrorCodes::UnsupportedFormat,
                          "samples collection is not clustered on _id");
        }

        const auto recordIdForId = [](std::string_view id) {
            return RecordIdBound(
                record_id_helpers::keyForObj(BSON(PersistentSampleDoc::k_idFieldName << id)));
        };

        auto exec = InternalPlanner::collectionScan(
            opCtx,
            collection,
            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
            InternalPlanner::FORWARD,
            /*resumeAfterRecordId=*/boost::none,
            recordIdForId(minId),
            recordIdForId(maxId),
            CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords);

        BSONObj pageDoc;
        while (exec->getNext(&pageDoc, nullptr) == PlanExecutor::ADVANCED) {
            pages.push_back(pageDoc.getOwned());
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return pages;
}

}  // namespace

std::string makePersistentSampleId(const UUID& collectionUuid,
                                   SamplingTechniqueEnum method,
                                   size_t requestedSampleSize,
                                   boost::optional<int> numChunks,
                                   int pageNo) {
    tassert(13321001, "Persistent sample page number must be non-negative", pageNo >= 0);

    const size_t width = pageNoWidth(requestedSampleSize);
    tassert(13321000,
            str::stream() << "Persistent sample page number " << pageNo
                          << " does not fit in the padding width " << width
                          << " implied by requested sample size " << requestedSampleSize,
            fmt::formatted_size("{}", pageNo) <= width);

    std::string id =
        makePersistentSampleIdPrefix(collectionUuid, method, requestedSampleSize, numChunks);
    fmt::format_to(std::back_inserter(id), "{:0{}}", pageNo, width);
    return id;
}

std::pair<std::string, std::string> makePersistentSampleIdRange(const UUID& collectionUuid,
                                                                SamplingTechniqueEnum method,
                                                                size_t requestedSampleSize,
                                                                boost::optional<int> numChunks) {
    // The bounds are the lowest and highest page-number components representable in the padding
    // width, so they bracket every page this sample could have without needing a sentinel page
    // number that no document uses.
    const size_t width = pageNoWidth(requestedSampleSize);
    std::string minId =
        makePersistentSampleIdPrefix(collectionUuid, method, requestedSampleSize, numChunks);
    std::string maxId = minId;
    minId.append(width, '0');
    maxId.append(width, '9');
    return {std::move(minId), std::move(maxId)};
}

StatusWith<PersistentSampleDoc> parsePersistentSample(const BSONObj& doc) {
    if (doc.isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, "persistent sample document is empty");
    }

    // Keep a local handle on the source buffer so we can hand each `docs` entry its own
    // SharedBuffer reference below.
    // `getOwned()` is a refcount bump if `doc` is already owned, a memcpy otherwise.
    const BSONObj ownedSource = doc.getOwned();

    PersistentSampleDoc parsed;
    try {
        parsed = PersistentSampleDoc::parseOwned(BSONObj(ownedSource),
                                                 IDLParserContext{"PersistentSample"});
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (parsed.getRequestedSamplingMethod() == SamplingTechniqueEnum::kChunk &&
        !parsed.getNumChunks().has_value()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "persistent sample 'numChunks' is required for chunk-technique samples");
    }
    if (parsed.getRequestedSamplingMethod() != SamplingTechniqueEnum::kChunk &&
        parsed.getNumChunks().has_value()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "persistent sample 'numChunks' must only be set for chunk-technique samples");
    }

    const auto requestedSampleSize = static_cast<size_t>(parsed.getRequestedSampleSize());
    // It is valid for `docs` to contain fewer entries than `requestedSampleSize`: this happens when
    // the collection is smaller than the requested sample size at analyze time or if the sample was
    // generated with chunking technique and some were starting less docs away from EOF than the
    // chunk size.
    if (parsed.getDocs().size() > requestedSampleSize) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream()
                          << "persistent sample 'docs' array length (" << parsed.getDocs().size()
                          << ") exceeds declared 'requestedSampleSize' (" << requestedSampleSize
                          << ")");
    }

    // The IDL parser materialises each `docs` entry as a BSONObj view into the parsed
    // PersistentSampleDoc's source buffer which is, in turn, shared with the ownedSource BSONObj we
    // created above. This is efficient but means the returned PersistentSampleDoc's `docs` entries
    // will dangle once the parsed struct goes out of scope so every consumer will need to get docs
    // owned which means copying them. To avoid that, we create a new vector of BSONObjs that
    // explicitly share ownership of the source buffer, and swap that into the parsed struct before
    // returning it. This way, callers can copy the returned `docs` entries and keep them alive
    // independently of the parsed struct's lifetime if they want to.
    std::vector<BSONObj> ownedDocs;
    ownedDocs.reserve(requestedSampleSize);
    for (const BSONObj& entry : parsed.getDocs()) {
        BSONObj owned = entry;
        owned.shareOwnershipWith(ownedSource);
        ownedDocs.push_back(std::move(owned));
    }
    parsed.setDocs(std::move(ownedDocs));

    return parsed;
}

StatusWith<PersistentSampleDoc> reassemblePersistentSample(std::vector<BSONObj> pages) {
    if (pages.empty()) {
        return Status(ErrorCodes::NoSuchKey, "no persistent sample pages found");
    }

    // Use the first page to set metadata for the full sample
    auto firstPage = parsePersistentSample(pages[0]);
    if (!firstPage.isOK()) {
        return firstPage.getStatus();
    }

    // Most samples will have only one page, so reserve enough space for the first page's docs to
    // start with. This provides the benefits of reserving space up front for most cases while not
    // over-allocating in cases where the actual number of sampled docs is less than the requested
    // sample size. Further inserts will have amortized O(1) complexity.
    size_t initialReservedSize = static_cast<size_t>(firstPage.getValue().getDocs().size());
    size_t totalMaxSampleSize = static_cast<size_t>(firstPage.getValue().getRequestedSampleSize());

    std::vector<BSONObj> allDocs;
    allDocs.reserve(initialReservedSize);
    allDocs.insert(allDocs.end(),
                   firstPage.getValue().getDocs().begin(),
                   firstPage.getValue().getDocs().end());

    PersistentSampleDoc reassembled = std::move(firstPage.getValue());

    // Parse and validate the rest of the pages, collecting their documents into the full sample.
    for (size_t i = 1; i < pages.size(); ++i) {
        auto parsed = parsePersistentSample(pages[i]);
        if (!parsed.isOK()) {
            return parsed.getStatus();
        }
        PersistentSampleDoc page = std::move(parsed.getValue());

        // The pages of a sample must form a contiguous run 0..N-1 arriving in pageNo order.
        if (page.getPageNo() != static_cast<int>(i)) {
            return Status(ErrorCodes::UnsupportedFormat,
                          str::stream()
                              << "persistent sample pages must be a contiguous "
                                 "run 0.."
                              << (pages.size() - 1) << " in order. Run is broken by page with _id: "
                              << page.get_id() << ", expected pageNo: " << i);
        }

        // All pages must agree on the sample's identity.
        if (page.getSchemaVersion() != reassembled.getSchemaVersion() ||
            page.getCollectionUuid() != reassembled.getCollectionUuid() ||
            page.getSamplingMethod() != reassembled.getSamplingMethod() ||
            page.getRequestedSamplingMethod() != reassembled.getRequestedSamplingMethod() ||
            page.getRequestedSampleSize() != reassembled.getRequestedSampleSize() ||
            page.getNumChunks() != reassembled.getNumChunks() ||
            page.getNumDocsDiscarded() != reassembled.getNumDocsDiscarded()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "persistent sample pages disagree on their identity fields");
        }

        // Concatenate this page's `docs`.
        const std::vector<BSONObj>& pageDocs = page.getDocs();
        allDocs.insert(allDocs.end(),
                       std::make_move_iterator(pageDocs.begin()),
                       std::make_move_iterator(pageDocs.end()));
        if (allDocs.size() > totalMaxSampleSize) {
            return Status(ErrorCodes::UnsupportedFormat,
                          str::stream()
                              << "reassembled persistent sample 'docs' array length ("
                              << allDocs.size() << ") exceeds declared 'requestedSampleSize' ("
                              << totalMaxSampleSize << ")");
        }
    }

    reassembled.setDocs(std::move(allDocs));
    return std::move(reassembled);
}

namespace {
auto& persistentSampleHits = *MetricBuilder<Counter64>{"query.sampling.persistentSample.hits"};
auto& persistentSampleMisses = *MetricBuilder<Counter64>{"query.sampling.persistentSample.misses"};

// Latency of successfully loading a persistent sample
auto& persistentSampleLoadMicros =
    *MetricBuilder<DurationCounter64<Microseconds>>{"query.sampling.persistentSample.loadMicros"};
// Histogram produces a vector bounds from 0.256 ms to 268000 ms (268 s)
auto& persistentSampleLoadMicrosHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{
        "query.sampling.persistentSample.histograms.loadMicros"}
         .bind(HistogramServerStatusMetric::computeBounds<uint64_t>(11, 256, 4));

auto& persistentSampleDocsLoaded =
    *MetricBuilder<Counter64>{"query.sampling.persistentSample.docsLoaded"};

// Histogram of pages read to reassemble a loaded persistent sample. Bounds are 1 to 32 pages.
auto& persistentSamplePagesReadHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{
        "query.sampling.persistentSample.histograms.pagesRead"}
         .bind(HistogramServerStatusMetric::computeBounds<uint64_t>(6, 1, 2));

// How old the sample was, at the moment it was read. Bounds are 1 second to ~388 days.
auto& persistentSampleAgeAtReadHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{
        "query.sampling.persistentSample.histograms.sampleAgeAtReadMillis"}
         .bind(HistogramServerStatusMetric::computeBounds<uint64_t>(26, 1000, 2));

// Only cases where a sample wasn't found are counted as misses, not cases of malformed
// content/samples collection.
void recordPersistentSampleFailure(const Status& status) {
    if (status.code() == ErrorCodes::NoSuchKey) {
        persistentSampleMisses.incrementRelaxed();
    }
}

// Records the metrics for a sample that was loaded successfully.
void recordPersistentSampleSuccess(TickSource* tickSource,
                                   TickSource::Tick startTicks,
                                   const Sample& sample,
                                   size_t pagesRead) {
    auto loadMicros = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTicks);
    persistentSampleLoadMicros.increment(loadMicros);
    persistentSampleLoadMicrosHistogram.increment(durationCount<Microseconds>(loadMicros));
    persistentSampleHits.incrementRelaxed();
    persistentSamplePagesReadHistogram.increment(pagesRead);
    const Date_t now = Date_t::now();
    const Date_t createdAt = sample.createdAt();
    // A sample stamped in the future normalized to 0 rather than a negative value.
    const Milliseconds age = now > createdAt ? now - createdAt : Milliseconds(0);
    persistentSampleAgeAtReadHistogram.increment(durationCount<Milliseconds>(age));
    persistentSampleDocsLoaded.incrementRelaxed(sample.size());
}
}  // namespace

std::unique_ptr<PersistentSampleLoader> makePersistentSampleLoader(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx) {
    return std::make_unique<PersistentSampleLoader>(
        opCtx, collections, yieldPolicy, std::move(customerQueryExpCtx));
}

StatusWith<Sample> PersistentSampleLoader::tryLoad(const SampleSpec& spec) {
    if (!feature_flags::gFeatureFlagPersistentStats.isEnabled(
            VersionContext::getDecoration(_opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return Status(ErrorCodes::NoSuchKey, "featureFlagPersistentStats is not enabled");
    }
    // TODO SERVER-131864: this should be a tassert rather than a status.
    if (spec.samplingSource != SamplingSourceEnum::kPersistentSample) {
        return Status(ErrorCodes::NoSuchKey, "sampling source is kOnTheFlySample");
    }

    const CollectionPtr& sourceCollection = _collections.lookupCollection(spec.nss);
    if (!sourceCollection) {
        return Status(ErrorCodes::NoSuchKey, "collection not found");
    }

    const SamplingTechniqueEnum method =
        resolveSamplingTechnique(spec, static_cast<size_t>(sourceCollection->numRecords(_opCtx)));
    const UUID collectionUuid = sourceCollection->uuid();
    const boost::optional<int> numChunks =
        (method == SamplingTechniqueEnum::kChunk) ? spec.numChunks : boost::none;

    const NamespaceString samplesNss =
        NamespaceStringUtil::deserialize(spec.nss.dbName(), kSamplesCollectionName);

    auto* tickSource = _opCtx->getServiceContext()->getTickSource();
    auto startTicks = tickSource->getTicks();

    // Perform a bounded range scan on the clustered samples collection to retrieve all pages in a
    // sample ordered by pageNo.
    const auto [minId, maxId] =
        makePersistentSampleIdRange(collectionUuid, method, spec.requestedSampleSize, numChunks);

    auto scanStatus = findPersistentSamplePages(_opCtx, samplesNss, minId, maxId);

    if (!scanStatus.isOK()) {
        recordPersistentSampleFailure(scanStatus.getStatus());
        return scanStatus.getStatus();
    }

    const size_t pagesRead = scanStatus.getValue().size();
    auto reassembled = reassemblePersistentSample(std::move(scanStatus.getValue()));

    if (!reassembled.isOK()) {
        recordPersistentSampleFailure(reassembled.getStatus());
        return reassembled.getStatus();
    }

    auto sample = Sample::makePersisted(reassembled.getValue().getDocs(),
                                        reassembled.getValue().getSamplingMethod(),
                                        reassembled.getValue().getCreatedAt(),
                                        pagesRead);

    recordPersistentSampleSuccess(tickSource, startTicks, sample, pagesRead);

    return sample;
}

}  // namespace mongo::ce
