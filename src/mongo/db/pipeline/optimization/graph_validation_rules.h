// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/pipeline.h"

namespace mongo::pipeline_optimization {

/**
 * Remove any previously inserted $_internalAssertDataAssumptions stages.
 */
void removeArraynessValidationStages(Pipeline& pipeline);

/**
 * Inserts $_internalAssertDataAssumptions stages before pipeline stages to validate the dependency
 * graph's arrayness analysis. This is a test-only feature controlled by the
 * internalEnableDependencyGraphValidation knob.
 */
void insertArraynessValidationStages(Pipeline& pipeline);

}  // namespace mongo::pipeline_optimization
