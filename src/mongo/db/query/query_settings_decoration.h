// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_settings.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class SharedCollectionDecorations;

/**
 * All Collection instances for the same collection share the same QuerySettings instance.
 * QuerySettingsDecoration decorates a decorable object that all Collection instances for the same
 * collection hold in shared ownership. See the Collection header file for more details.
 */
class QuerySettingsDecoration {
public:
    /**
     * Fetches a pointer to the QuerySettings from the collection's 'decorations'.
     */
    static QuerySettings* get(SharedCollectionDecorations* decorations);

    QuerySettingsDecoration();

private:
    // Query settings for a collection.
    std::unique_ptr<QuerySettings> _querySettings;
};

}  // namespace mongo
