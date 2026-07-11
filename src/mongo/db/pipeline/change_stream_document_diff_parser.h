// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace change_stream_document_diff_parser {
struct DeltaUpdateDescription {
    DeltaUpdateDescription(const DeltaUpdateDescription& other) = delete;
    DeltaUpdateDescription(DeltaUpdateDescription&& other) = default;
    DeltaUpdateDescription() = default;

    Document updatedFields;
    std::vector<Value> removedFields;
    std::vector<Value> truncatedArrays;
    Document disambiguatedPaths;
};

/**
 * Parses a document diff to generate update fields in the format required by the change streams.
 */
DeltaUpdateDescription parseDiff(const doc_diff::Diff& diff);

}  // namespace change_stream_document_diff_parser
}  // namespace mongo
