// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/matchable.h"

#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

namespace mongo {

namespace {
// Upper bound on the document size (in bytes) for which the 'match against the whole trivially
// convertible document' fast path (see DocumentMatchableDocument::documentToBSON) is used. Bounds
// the cost of the matcher's linear field scan over fields the predicate does not need.
constexpr int kWholeDocumentMatchMaxSizeBytes = 16 * 1024;
}  // namespace

BSONMatchableDocument::BSONMatchableDocument(const BSONObj& obj) : _obj(obj) {
    _iteratorUsed = false;
}

BSONMatchableDocument::~BSONMatchableDocument() {}

DocumentMatchableDocument::DocumentMatchableDocument(const Document& doc,
                                                     const DepsTracker& dependencies,
                                                     bool knownUniqueFields)
    : BSONMatchableDocument(documentToBSON(doc, dependencies, knownUniqueFields)), _doc(doc) {}

// MatchExpression only takes BSON documents, so we have to make one. As an optimization,
// only serialize the fields we need to do the match. Specify BSONObj::LargeSizeTrait so
// that matching against a large document mid-pipeline does not throw a BSON max-size error.
BSONObj DocumentMatchableDocument::documentToBSON(const Document& doc,
                                                  const DepsTracker& dependencies,
                                                  bool knownUniqueFields) {
    if (dependencies.needWholeDocument) {
        return doc.toBson<BSONObj::LargeSizeTrait>();
    }
    // Fast path: if the document is already backed by owned BSON (e.g. a document materialized
    // by a SequentialDocumentCache), matching against the whole document avoids rebuilding a
    // projected BSON. This is a win when the same document is matched repeatedly (a nested-loop
    // $lookup re-scanning its cached prefix). Correctness is unchanged -- the matcher only
    // reads the paths it needs -- and the size gate bounds the extra field-scan cost for wide
    // docs.
    if (auto whole = doc.toBsonIfTriviallyConvertible();
        whole && whole->objsize() <= kWholeDocumentMatchMaxSizeBytes) {
        return std::move(*whole);
    }
    if (knownUniqueFields) {
        // Use optimized function that does not check whether we have already seen a specific
        // first field.
        return document_path_support::documentToBsonWithPaths<
            BSONObj::LargeSizeTrait,
            /* PathsHaveUniqueFirstFields */ true>(doc, dependencies.fields);
    }

    // Use slow function that will check for first field uniqueness.
    return document_path_support::documentToBsonWithPaths<BSONObj::LargeSizeTrait,
                                                          /* PathsHaveUniqueFirstFields */ false>(
        doc, dependencies.fields);
}

}  // namespace mongo
