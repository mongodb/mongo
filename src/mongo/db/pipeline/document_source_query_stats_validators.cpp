// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_query_stats_validators.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/util/str.h"

namespace mongo {

Status validateAlgo(TransformAlgorithmEnum algorithm) {
    if (algorithm == TransformAlgorithmEnum::kNone) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Algorithm specified but found no valid algorithm"};
    }
    return Status::OK();
}

Status validateHmac(std::vector<uint8_t> hmacKey) {
    if (hmacKey.size() < 32) {
        return {ErrorCodes::BadValue,
                str::stream() << "hmacKey must be greater than or equal to 32 bytes, found length: "
                              << hmacKey.size()};
    }
    // length check
    return Status::OK();
}
}  // namespace mongo
