// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] ListCollectionsFilter {
    ListCollectionsFilter(const ListCollectionsFilter&) = delete;
    ListCollectionsFilter& operator=(const ListCollectionsFilter&) = delete;

public:
    /**
     * Returns an object that can be passed to the listCollections command as a filter to limit
     * results to collection namespaces.
     */
    static BSONObj makeTypeCollectionFilter();

    /**
     * Returns an object that can be passed to the listCollections command as a filter to limit
     * results to view namespaces.
     */
    static BSONObj makeTypeViewFilter();

    /**
     * Returns an object that can be passed to the listCollections command as a filter to exclude
     * view namespaces (type !== "view").
     */
    static BSONObj makeExcludeViewsFilter();

    /**
     * Injects into an existing listCollections filter a clause to limit results to collection
     * namespaces.
     */
    static BSONObj addTypeCollectionFilter(const BSONObj& filter);

    /**
     * Injects into an existing listCollections filter a clause to limit results to view
     * namespaces.
     */
    static BSONObj addTypeViewFilter(const BSONObj& filter);
};

}  // namespace mongo
