// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"

namespace mongo::rule_based_rewrites::pipeline {

REGISTER_RULES(DocumentSourceVectorSearch, OPTIMIZE_AT_RULE(DocumentSourceVectorSearch));
REGISTER_RULES(DocumentSourceInternalSetWindowFields,
               OPTIMIZE_AT_RULE(DocumentSourceInternalSetWindowFields),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceInternalSetWindowFields));
REGISTER_RULES(DocumentSourceGeoNear,
               OPTIMIZE_AT_RULE(DocumentSourceGeoNear),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceGeoNear));
REGISTER_RULES(DocumentSourceInternalSearchIdLookUp,
               OPTIMIZE_AT_RULE(DocumentSourceInternalSearchIdLookUp));
REGISTER_RULES(DocumentSourceSearch, OPTIMIZE_AT_RULE(DocumentSourceSearch));
REGISTER_RULES(DocumentSourceInternalDensify, OPTIMIZE_AT_RULE(DocumentSourceInternalDensify));
REGISTER_RULES(DocumentSourceInternalDocumentResultsAndMetadata,
               OPTIMIZE_AT_RULE(DocumentSourceInternalDocumentResultsAndMetadata));

}  // namespace mongo::rule_based_rewrites::pipeline
