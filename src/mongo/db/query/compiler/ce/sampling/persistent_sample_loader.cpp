// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
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
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::ce {

BSONObj makePersistentSampleIdObj(const UUID& collectionUuid,
                                  SamplingTechniqueEnum method,
                                  size_t sampleSize,
                                  boost::optional<int> numChunks,
                                  int pageNo) {
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

    PersistentSampleId id;
    id.setSchemaVersion(kPersistentSampleSchemaVersion);
    id.setCollectionUuid(collectionUuid);
    id.setSamplingMethod(method);
    id.setSampleSize(static_cast<long long>(sampleSize));
    if (method == SamplingTechniqueEnum::kChunk) {
        id.setNumChunks(*numChunks);
    }
    id.setPageNo(pageNo);
    return id.toBSON();
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

    // The identity fields are stored both inside the structured `_id` and as top-level fields;
    // a mismatch means a corrupt persisted sample.
    const PersistentSampleId& id = parsed.get_id();
    if (id.getSchemaVersion() != parsed.getSchemaVersion() ||
        id.getCollectionUuid().toString() != parsed.getCollectionUuid() ||
        id.getSamplingMethod() != parsed.getSamplingMethod() ||
        id.getNumChunks() != parsed.getNumChunks()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "persistent sample '_id' identity fields do not match the document's "
                      "top-level fields");
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
        if (page.get_id().getPageNo() != static_cast<int>(i)) {
            return Status(ErrorCodes::UnsupportedFormat,
                          str::stream()
                              << "persistent sample pages must be a contiguous "
                                 "run 0.."
                              << (pages.size() - 1) << " in order. Run is broken by page with _id: "
                              << page.get_id().toBSON() << ", expected pageNo: " << i);
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

StatusWith<PersistentSampleDoc> PersistentSampleLoader::tryLoad(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const UUID& collectionUuid,
    SamplingTechniqueEnum method,
    size_t sampleSize,
    boost::optional<int> numChunks) const {
    const NamespaceString nss = NamespaceStringUtil::deserialize(dbName, kSamplesCollectionName);

    // Perform a bounded range scan on the clustered samples collection to retrieve all pages in a
    // sample ordered by pageNo.
    using PageNoType = decltype(std::declval<PersistentSampleId>().getPageNo());
    const BSONObj minId =
        makePersistentSampleIdObj(collectionUuid, method, sampleSize, numChunks, /*pageNo=*/0);
    const BSONObj maxId =
        makePersistentSampleIdObj(collectionUuid,
                                  method,
                                  sampleSize,
                                  numChunks,
                                  /*pageNo=*/std::numeric_limits<PageNoType>::max());

    const auto recordIdForId = [](const BSONObj& id) {
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

    return reassemblePersistentSample(std::move(pages));
}

}  // namespace mongo::ce
