// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/document_source_internal_replace_root.h"
#include "mongo/db/pipeline/document_source_list_mql_entities.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

namespace mongo::rule_based_rewrites::pipeline {

REGISTER_RULES(DocumentSourceSkip,
               OPTIMIZE_AT_RULE(DocumentSourceSkip),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceSkip));
REGISTER_RULES(DocumentSourceListMqlEntities, OPTIMIZE_AT_RULE(DocumentSourceListMqlEntities));
REGISTER_RULES(DocumentSourceLimit, OPTIMIZE_AT_RULE(DocumentSourceLimit));
REGISTER_RULES(DocumentSourceGroup,
               OPTIMIZE_AT_RULE(DocumentSourceGroup),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceGroup));
REGISTER_RULES(DocumentSourceUnionWith,
               OPTIMIZE_AT_RULE(DocumentSourceUnionWith),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceUnionWith));
REGISTER_RULES(DocumentSourcePlanCacheStats, OPTIMIZE_AT_RULE(DocumentSourcePlanCacheStats));
REGISTER_RULES(DocumentSourceUnwind, OPTIMIZE_AT_RULE(DocumentSourceUnwind));
REGISTER_RULES(DocumentSourceInternalReplaceRoot,
               OPTIMIZE_AT_RULE(DocumentSourceInternalReplaceRoot));
REGISTER_RULES(DocumentSourceGraphLookUp, OPTIMIZE_AT_RULE(DocumentSourceGraphLookUp));
REGISTER_RULES(DocumentSourceInternalProjection,
               OPTIMIZE_AT_RULE(DocumentSourceInternalProjection));
REGISTER_RULES(DocumentSourceSort, OPTIMIZE_AT_RULE(DocumentSourceSort));
REGISTER_RULES(DocumentSourceBucketAuto, OPTIMIZE_IN_PLACE_RULE(DocumentSourceBucketAuto));
REGISTER_RULES(DocumentSourceFacet, OPTIMIZE_IN_PLACE_RULE(DocumentSourceFacet));
REGISTER_RULES(DocumentSourceStreamingGroup, OPTIMIZE_IN_PLACE_RULE(DocumentSourceStreamingGroup));

}  // namespace mongo::rule_based_rewrites::pipeline
