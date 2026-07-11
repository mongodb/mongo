// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/search/document_source_list_search_indexes_gen.h"

namespace mongo {

void validateListSearchIndexesSpec(const DocumentSourceListSearchIndexesSpec* spec) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot set both 'name' and 'id' for $listSearchIndexes.",
            !(spec->getId() && spec->getName()));
};

}  // namespace mongo
