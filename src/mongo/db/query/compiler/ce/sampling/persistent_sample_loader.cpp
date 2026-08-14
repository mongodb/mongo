// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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
 * Separator between the parts of a persistent sample `_id`. Must sort strictly greater than every
 * digit so that a sample whose identity prefix is a numeric prefix of another's (e.g. sampleSize
 * 1 vs 10) sorts outside the latter's page range rather than inside it.
 */
constexpr char kIdSeparator = '_';
static_assert(kIdSeparator > '9');

/**
 * Number of digits the page number is zero-padded to for a sample of the given size.
 */
size_t pageNoWidth(size_t sampleSize) {
    return fmt::formatted_size("{}", sampleSize);
}

/**
 * Builds the part of the `_id` shared by every page of a sample, up to and including the separator
 * that precedes the page number.
 */
std::string makePersistentSampleIdPrefix(const UUID& collectionUuid,
                                         SamplingTechniqueEnum method,
                                         size_t sampleSize,
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
    tassert(13102000, "Persistent sample ID requires a positive sample size", sampleSize > 0);

    std::string prefix = fmt::format("{}{}{}{}{}{}{}{}",
                                     collectionUuid.toString(),
                                     kIdSeparator,
                                     kPersistentSampleSchemaVersion,
                                     kIdSeparator,
                                     idlSerialize(method),
                                     kIdSeparator,
                                     sampleSize,
                                     kIdSeparator);
    if (numChunks) {
        fmt::format_to(std::back_inserter(prefix), "{}{}", *numChunks, kIdSeparator);
    }
    return prefix;
}

/**
 * Appends every field of a sample page document except the `docs` array, leaving `builder` ready
 * for the caller to append that array.
 */
void appendSamplePageMetadata(BSONObjBuilder& builder,
                              const UUID& collectionUuid,
                              SamplingTechniqueEnum method,
                              size_t sampleSize,
                              boost::optional<int> numChunks,
                              int pageNo,
                              Date_t createdAt) {
    const std::string id =
        makePersistentSampleId(collectionUuid, method, sampleSize, numChunks, pageNo);

    builder.append(PersistentSampleDoc::k_idFieldName, id);
    builder.append(PersistentSampleDoc::kPageNoFieldName, pageNo);
    builder.append(PersistentSampleDoc::kCollectionUuidFieldName, collectionUuid.toString());
    builder.append(PersistentSampleDoc::kSchemaVersionFieldName, kPersistentSampleSchemaVersion);
    builder.appendDate(PersistentSampleDoc::kCreatedAtFieldName, createdAt);
    builder.append(PersistentSampleDoc::kSampleSizeFieldName, static_cast<long long>(sampleSize));
    builder.append(PersistentSampleDoc::kSamplingMethodFieldName, idlSerialize(method));
    if (method == SamplingTechniqueEnum::kChunk) {
        builder.append(PersistentSampleDoc::kNumChunksFieldName, *numChunks);
    }
}


}  // namespace

std::string makePersistentSampleId(const UUID& collectionUuid,
                                   SamplingTechniqueEnum method,
                                   size_t sampleSize,
                                   boost::optional<int> numChunks,
                                   int pageNo) {
    tassert(13321001, "Persistent sample page number must be non-negative", pageNo >= 0);

    const size_t width = pageNoWidth(sampleSize);
    tassert(13321000,
            str::stream() << "Persistent sample page number " << pageNo
                          << " does not fit in the padding width " << width
                          << " implied by sample size " << sampleSize,
            fmt::formatted_size("{}", pageNo) <= width);

    std::string id = makePersistentSampleIdPrefix(collectionUuid, method, sampleSize, numChunks);
    fmt::format_to(std::back_inserter(id), "{:0{}}", pageNo, width);
    return id;
}

std::pair<std::string, std::string> makePersistentSampleIdRange(const UUID& collectionUuid,
                                                                SamplingTechniqueEnum method,
                                                                size_t sampleSize,
                                                                boost::optional<int> numChunks) {
    // The bounds are the lowest and highest page-number components representable in the padding
    // width, so they bracket every page this sample could have without needing a sentinel page
    // number that no document uses.
    const size_t width = pageNoWidth(sampleSize);
    std::string minId = makePersistentSampleIdPrefix(collectionUuid, method, sampleSize, numChunks);
    std::string maxId = minId;
    minId.append(width, '0');
    maxId.append(width, '9');
    return {std::move(minId), std::move(maxId)};
}

std::vector<BSONObj> makePersistentSamplePageDocs(const UUID& collectionUuid,
                                                  SamplingTechniqueEnum method,
                                                  size_t sampleSize,
                                                  boost::optional<int> numChunks,
                                                  const std::vector<BSONObj>& sample,
                                                  Date_t createdAt) {
    std::vector<BSONObj> pages;
    size_t docIdx = 0;
    int pageNo = 0;
    size_t numDiscarded = 0;

    const double maxDiscardFraction = internalQueryMaxPersistentSampleDiscardFraction.load();
    do {
        BSONObjBuilder builder;
        appendSamplePageMetadata(
            builder, collectionUuid, method, sampleSize, numChunks, pageNo, createdAt);
        BSONArrayBuilder docsArr(builder.subarrayStart(PersistentSampleDoc::kDocsFieldName));

        // Size of this page with its metadata but no documents.
        const int emptyPageLenBytes = builder.len();

        int docsOnPage = 0;
        while (docIdx < sample.size()) {
            const BSONObj& doc = sample[docIdx];

            // Calculate the maximum number of bytes this document would add to a page.
            const int docSizeWithOverheadBytes =
                doc.objsize() + kArrayElementMaxOverheadBytes + kBytesToClosePage;

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
        // TODO SERVER-127501
        if (docsOnPage == 0 && pageNo > 0) {
            break;
        }

        BSONObj page = builder.obj();
        tassert(13044800,
                str::stream() << "Assembled sample page of " << page.objsize()
                              << " bytes exceeds the maximum BSON size of " << BSONObjMaxUserSize
                              << " bytes",
                page.objsize() <= BSONObjMaxUserSize);
        pages.push_back(std::move(page));
        ++pageNo;
    } while (docIdx < sample.size());

    return pages;
}

BSONObj makePersistentSampleAllPagesLookupFilter(const UUID& collectionUuid,
                                                 SamplingTechniqueEnum method,
                                                 size_t sampleSize,
                                                 boost::optional<int> numChunks) {
    const auto [minId, maxId] =
        makePersistentSampleIdRange(collectionUuid, method, sampleSize, numChunks);
    return BSON(PersistentSampleDoc::k_idFieldName << BSON("$gte" << minId << "$lte" << maxId));
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

    if (parsed.getSamplingMethod() == SamplingTechniqueEnum::kChunk &&
        !parsed.getNumChunks().has_value()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "persistent sample 'numChunks' is required for chunk-technique samples");
    }
    if (parsed.getSamplingMethod() != SamplingTechniqueEnum::kChunk &&
        parsed.getNumChunks().has_value()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "persistent sample 'numChunks' must only be set for chunk-technique samples");
    }

    const auto sampleSize = static_cast<size_t>(parsed.getSampleSize());
    // It is valid for `docs` to contain fewer entries than `sampleSize`: this happens when the
    // collection is smaller than the requested sample size at analyze time or if the sample was
    // generated with chunking technique and some were starting less docs away from EOF than the
    // chunk size.
    if (parsed.getDocs().size() > sampleSize) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream()
                          << "persistent sample 'docs' array length (" << parsed.getDocs().size()
                          << ") exceeds declared 'sampleSize' (" << sampleSize << ")");
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
    ownedDocs.reserve(sampleSize);
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
    size_t totalMaxSampleSize = static_cast<size_t>(firstPage.getValue().getSampleSize());

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
            page.getSampleSize() != reassembled.getSampleSize() ||
            page.getNumChunks() != reassembled.getNumChunks()) {
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
                          str::stream() << "reassembled persistent sample 'docs' array length ("
                                        << allDocs.size() << ") exceeds declared 'sampleSize' ("
                                        << totalMaxSampleSize << ")");
        }
    }

    reassembled.setDocs(std::move(allDocs));
    return std::move(reassembled);
}

namespace {
// Histogram of pages read to reassemble a loaded persistent sample. Bounds are 1 to 32 pages.
auto& persistentSamplePagesReadHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{
        "query.sampling.persistentSample.histograms.pagesRead"}
         .bind(HistogramServerStatusMetric::pow(6, 1, 2));

// How old the sample was, at the moment it was read. Bounds are 1 second to ~388 days.
auto& persistentSampleAgeAtReadHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{
        "query.sampling.persistentSample.histograms.sampleAgeAtReadMillis"}
         .bind(HistogramServerStatusMetric::pow(26, 1000, 2));
}  // namespace

StatusWith<LoadedPersistentSample> PersistentSampleLoader::tryLoad(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const UUID& collectionUuid,
    SamplingTechniqueEnum method,
    size_t sampleSize,
    boost::optional<int> numChunks) const {
    const NamespaceString nss = NamespaceStringUtil::deserialize(dbName, kSamplesCollectionName);

    // Perform a bounded range scan on the clustered samples collection to retrieve all pages in a
    // sample ordered by pageNo.
    const auto [minId, maxId] =
        makePersistentSampleIdRange(collectionUuid, method, sampleSize, numChunks);

    const auto recordIdForId = [](std::string_view id) {
        return RecordIdBound(
            record_id_helpers::keyForObj(BSON(PersistentSampleDoc::k_idFieldName << id)));
    };

    std::vector<BSONObj> pages;
    try {
        const auto collection = acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead));
        if (!collection.exists()) {
            return Status(ErrorCodes::NoSuchKey, "samples collection does not exist");
        }

        if (!collection.getCollectionPtr()->isClustered()) {
            return Status(ErrorCodes::NoSuchKey, "samples collection is not clustered on _id");
        }

        auto exec = InternalPlanner::collectionScan(
            opCtx,
            collection,
            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
            InternalPlanner::FORWARD,
            boost::none /* resumeAfterRecordId */,
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

    const size_t pagesRead = pages.size();
    auto reassembled = reassemblePersistentSample(std::move(pages));
    if (!reassembled.isOK()) {
        return reassembled.getStatus();
    }

    // Metrics are recorded on success.
    persistentSamplePagesReadHistogram.increment(pagesRead);
    const Date_t now = Date_t::now();
    const Date_t createdAt = reassembled.getValue().getCreatedAt();
    // A sample stamped in the future normalized to 0 rather than a negative value.
    const Milliseconds age = now > createdAt ? now - createdAt : Milliseconds(0);
    persistentSampleAgeAtReadHistogram.increment(durationCount<Milliseconds>(age));

    return LoadedPersistentSample{std::move(reassembled.getValue()), pagesRead};
}

}  // namespace mongo::ce
