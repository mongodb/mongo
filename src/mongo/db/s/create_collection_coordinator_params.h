/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"


namespace mongo {

/**
 * An extension of the CreateCollectionRequest parameters received by the Coordinator including
 * methods to resolve the user request against the current state of the DB catalog and safely access
 * the outcome.
 */
class CreateCollectionCoordinatorParams : public CreateCollectionRequest {
public:
    CreateCollectionCoordinatorParams(const CreateCollectionRequest& request,
                                      const NamespaceString& targetedNamespace);

    /*
     * Resolution method to be invoked before accessing any of the request fields. It assumes that
     * the caller has already acquired the needed resources to ensure that the catalog can be
     * safely accessed.
     */
    void resolveAgainstLocalCatalog(OperationContext* opCtx);

    const NamespaceString& getNameSpaceToShard() const;

    const ShardKeyPattern& getShardKeyPattern() const;

    const boost::optional<TimeseriesOptions>& getTimeseries() const;

    BSONObj getResolvedCollation() const;

    boost::optional<TimeseriesOptions>& getTimeseries();

private:
    bool _resolutionPerformed;
    NamespaceString _originalNamespace;
    NamespaceString _resolvedNamespace;
    boost::optional<ShardKeyPattern> _shardKeyPattern;
    BSONObj _resolvedCollation;

    // Hide harmful non-virtual methods defined by the parent class

    void setShardKey(boost::optional<BSONObj> value) {
        MONGO_UNREACHABLE;
    }

    const boost::optional<BSONObj>& getCollation() const {
        MONGO_UNREACHABLE;
    }

    void setCollation(boost::optional<BSONObj> value) {
        MONGO_UNREACHABLE;
    }
};

}  // namespace mongo
