// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

namespace mongo {

// Calls normalizeMatchExpression for any DocumentSourceMatch within the pipeline.
std::unique_ptr<Pipeline> normalizeMatchStageInPipeline(std::unique_ptr<Pipeline> pipeline);

}  // namespace mongo
