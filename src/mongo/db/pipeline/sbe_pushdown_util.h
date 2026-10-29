// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"

namespace mongo {
class DocumentSourceGroupBase;

/**
 * Set the minimum required compatibility based on the 'featureFlagSbeFull' and the
 * query framework control knob. If 'featureFlagSbeFull' is true, set the compatibility to
 * 'requiresTrySbe'; otherwise, if query framework control knob is 'trySbeEngine', set
 * compatibility to 'requiresTrySbe'; otherwise set it to 'noRequirements'.
 */
SbeCompatibility getMinRequiredSbeCompatibility(QueryFrameworkControlEnum currentQueryKnobFramework,
                                                bool sbeFullEnabled);

bool isGroupSbeCompatible(const DocumentSourceGroupBase& group,
                          SbeCompatibility minRequiredCompatibility);
}  // namespace mongo
