// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

namespace mongo::rule_based_rewrites::pipeline {

REGISTER_RULES(DocumentSourceSequentialDocumentCache,
               OPTIMIZE_AT_RULE(DocumentSourceSequentialDocumentCache));

}  // namespace mongo::rule_based_rewrites::pipeline
