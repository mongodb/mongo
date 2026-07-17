// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo::ce {

BSONObj makePersistentSampleIdObj(const UUID& collectionUuid,
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

    PersistentSampleId id;
    id.setSchemaVersion(kPersistentSampleSchemaVersion);
    id.setCollectionUuid(collectionUuid);
    id.setSamplingMethod(method);
    id.setSampleSize(static_cast<long long>(sampleSize));
    if (method == SamplingTechniqueEnum::kChunk) {
        id.setNumChunks(*numChunks);
    }
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

StatusWith<PersistentSampleDoc> PersistentSampleLoader::tryLoad(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const UUID& collectionUuid,
    SamplingTechniqueEnum method,
    size_t sampleSize,
    boost::optional<int> numChunks) const {
    const NamespaceString nss = NamespaceStringUtil::deserialize(dbName, kSamplesCollectionName);
    const BSONObj idObj = makePersistentSampleIdObj(collectionUuid, method, sampleSize, numChunks);

    BSONObj doc;
    try {
        DBDirectClient client(opCtx);
        // No projection is passed here intentionally to ensure query is express-eligible.
        doc = client.findOne(nss, BSON("_id" << idObj));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return parsePersistentSample(doc);
}

}  // namespace mongo::ce
