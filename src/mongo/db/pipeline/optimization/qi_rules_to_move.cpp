/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
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

}  // namespace mongo::rule_based_rewrites::pipeline
