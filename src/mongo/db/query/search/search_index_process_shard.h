// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/search/search_index_process_interface.h"
#include "mongo/util/modules.h"

namespace mongo {

class SearchIndexProcessShard : public SearchIndexProcessInterface {
public:
    std::pair<boost::optional<UUID>, boost::optional<ResolvedNamespace>>
    fetchCollectionUUIDAndResolveView(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      bool failOnTsColl = true) override;
    std::pair<UUID, boost::optional<ResolvedNamespace>> fetchCollectionUUIDAndResolveViewOrThrow(
        OperationContext* opCtx, const NamespaceString& nss) override;
};

}  // namespace mongo
