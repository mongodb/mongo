/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <fmt/format.h>

namespace mongo::ce {

std::string buildPersistentSampleId(const UUID& collectionUuid,
                                    SamplingCEMethodEnum method,
                                    size_t sampleSize,
                                    boost::optional<int> numChunks) {
    std::string methodStr;
    if (method == SamplingCEMethodEnum::kChunk) {
        tassert(
            12432800, "Chunk-based persistent sample ID requires numChunks", numChunks.has_value());
        methodStr = fmt::format("chunk{}", *numChunks);
    } else {
        tassert(12432801,
                "numChunks must only be set for chunk-technique persistent samples",
                !numChunks.has_value());
        methodStr = std::string(idlSerialize(method));
    }
    return fmt::format("{}_{}_{}_v{}",
                       collectionUuid.toString(),
                       methodStr,
                       sampleSize,
                       kPersistentSampleSchemaVersion);
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

    if (parsed.getSamplingMethod() == SamplingCEMethodEnum::kChunk &&
        !parsed.getNumChunks().has_value()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "persistent sample 'numChunks' is required for chunk-technique samples");
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
    SamplingCEMethodEnum method,
    size_t sampleSize,
    boost::optional<int> numChunks) const {
    const NamespaceString nss = NamespaceStringUtil::deserialize(dbName, kSamplesCollectionName);
    const std::string id = buildPersistentSampleId(collectionUuid, method, sampleSize, numChunks);

    BSONObj doc;
    try {
        DBDirectClient client(opCtx);
        // No projection is passed here intentionally to ensure query is express-eligible.
        doc = client.findOne(nss, BSON("_id" << id));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return parsePersistentSample(doc);
}

}  // namespace mongo::ce
