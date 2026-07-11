// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_hybrid_scoring_input_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"

namespace mongo {

Status validatePipelinesObject(const BSONObj& pipelines) {
    if (pipelines.isEmpty()) {
        return {ErrorCodes::BadValue,
                "A hybrid scoring stage should be run with at least one pipeline."};
    }

    for (auto&& elem : pipelines) {
        if (auto status = attemptToParsePipelineFromBSON(elem).getStatus(); !status.isOK()) {
            return status.withContext("Error parsing $rankFusion/$scoreFusion.input.pipelines");
        }
    }

    return Status::OK();
}
}  // namespace mongo
