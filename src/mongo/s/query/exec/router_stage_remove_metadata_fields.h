// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {

struct RemoveAllMetadataFieldsPolicy {
    static bool shouldRemove(std::string_view name) {
        return Document::isMetadataFieldName(name);
    }
};

struct RemoveSortKeyPolicy {
    static bool shouldRemove(std::string_view name) {
        return name.starts_with('$') && name == Document::metaFieldSortKey;
    }
};

/**
 * Removes metadata fields from a BSON object.
 */
template <typename RemovePolicy>
class RouterStageRemoveFields final : public RouterExecStage {
public:
    RouterStageRemoveFields(OperationContext* opCtx, std::unique_ptr<RouterExecStage> child);

    StatusWith<ClusterQueryResult> next() final;

    bool isEOF() const final {
        return getChildStage()->isEOF();
    }
};

using RouterStageRemoveMetadataFields = RouterStageRemoveFields<RemoveAllMetadataFieldsPolicy>;
using RouterStageRemoveSortKey = RouterStageRemoveFields<RemoveSortKeyPolicy>;

}  // namespace mongo
