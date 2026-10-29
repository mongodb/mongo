// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/document_source_internal_list_collections.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

namespace mongo::rule_based_rewrites::pipeline {

REGISTER_RULES(DocumentSourceInternalListCollections,
               OPTIMIZE_AT_RULE(DocumentSourceInternalListCollections));
REGISTER_RULES(DocumentSourceInternalAllCollectionStats,
               OPTIMIZE_AT_RULE(DocumentSourceInternalAllCollectionStats));

}  // namespace mongo::rule_based_rewrites::pipeline
