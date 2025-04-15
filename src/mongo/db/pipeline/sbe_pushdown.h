/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/pipeline/expression_context.h"

namespace mongo {
class CanonicalQuery;
class MultipleCollectionAccessor;
class Pipeline;

/**
 * Removes the first 'stagesToRemove' stages from the pipeline. This function is meant to be paired
 * with a call to attachPipelineStages() - the caller must first get the stages for push down, add
 * them to the canonical query, and only then remove them from the pipeline.
 */
void finalizePipelineStages(Pipeline* pipeline, CanonicalQuery* canonicalQuery);

/**
 * Identifies the prefix of the 'pipeline' that is eligible for running in SBE and adds it to the
 * provided 'canonicalQuery'.
 */
void attachPipelineStages(const MultipleCollectionAccessor& collections,
                          const Pipeline* pipeline,
                          bool needsMerge,
                          CanonicalQuery* canonicalQuery);

/**
 * Set the minimum required compatibility based on the 'featureFlagSbeFull' and the
 * query framework control knob. If 'featureFlagSbeFull' is true, set the compatibility to
 * 'requiresSbeFull'; otherwise,  if query framework control knob is 'trySbeEngine', set
 * compatibility to 'requiresTrySbe'; otherwise set it to 'noRequirements'.
 */
SbeCompatibility getMinRequiredSbeCompatibility(QueryFrameworkControlEnum currentQueryKnobFramework,
                                                bool sbeFullEnabled);
}  // namespace mongo
