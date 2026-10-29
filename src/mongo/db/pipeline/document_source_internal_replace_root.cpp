// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_replace_root.h"

#include <string_view>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(_internalReplaceRoot, DocumentSourceInternalReplaceRoot::id)

std::string_view DocumentSourceInternalReplaceRoot::getSourceName() const {
    return kStageNameInternal;
}

DocumentSourceContainer::iterator DocumentSourceInternalReplaceRoot::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282991, "Expecting DocumentSource iterator pointing to this stage", *itr == this);
    return itr;
}

Value DocumentSourceInternalReplaceRoot::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _newRoot->serialize()}});
}
}  // namespace mongo
