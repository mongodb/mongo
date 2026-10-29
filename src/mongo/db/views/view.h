// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

namespace mongo {

/**
 * Represents a "view": a virtual collection defined by a query on a collection or another view.
 */
class [[MONGO_MOD_PUBLIC]] ViewDefinition {
public:
    /**
     * In the database 'dbName', create a new view 'viewName' on the view or collection
     * 'viewOnName'. Neither 'viewName' nor 'viewOnName' should include the name of the database.
     */
    ViewDefinition(const DatabaseName& dbName,
                   std::string_view viewName,
                   std::string_view viewOnName,
                   const BSONObj& pipeline,
                   std::unique_ptr<CollatorInterface> collation);

    /**
     * Copying a view 'other' clones its collator and does a simple copy of all other fields.
     */
    ViewDefinition(const ViewDefinition& other);
    ViewDefinition& operator=(const ViewDefinition& other);

    /**
     * @return The fully-qualified namespace of this view.
     */
    const NamespaceString& name() const {
        return _viewNss;
    }

    /**
     * @return The fully-qualified namespace of the view or collection upon which this view is
     * based.
     */
    const NamespaceString& viewOn() const {
        return _viewOnNss;
    }

    /**
     * Returns a vector of BSONObjs that represent the stages of the aggregation pipeline that
     * defines this view.
     */
    const std::vector<BSONObj>& pipeline() const {
        return _pipeline;
    }

    /**
     * Returns the default collator for this view.
     */
    const CollatorInterface* defaultCollator() const {
        return _collator.get();
    }

    /**
     * Returns 'true' if this view is a time-series collection. That is, it is backed by a
     * time-series buckets collection.
     */
    bool timeseries() const {
        return _viewOnNss.isTimeseriesBucketsCollection();
    }

    void setViewOn(const NamespaceString& viewOnNss);

    /**
     * Pipeline must be of type array.
     */
    void setPipeline(std::vector<mongo::BSONObj> pipeline);

private:
    NamespaceString _viewNss;
    NamespaceString _viewOnNss;
    std::unique_ptr<CollatorInterface> _collator;
    std::vector<BSONObj> _pipeline;
};
}  // namespace mongo
