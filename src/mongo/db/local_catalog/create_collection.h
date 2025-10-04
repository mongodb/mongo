/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;

/**
 * Creates a collection as described in "cmdObj" on the database "dbName". Creates the collection's
 * _id index according to 'idIndex', if it is non-empty. When 'idIndex' is empty, creates the
 * default _id index.
 */
Status createCollection(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex = BSONObj());

/**
 * Creates a collection as parsed in 'cmd'.
 */
Status createCollection(OperationContext* opCtx, const CreateCommand& cmd);

/**
 * Creates the collection or the view as described by 'options'.
 */
Status createCollection(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const CollectionOptions& options,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier = boost::none);

/**
 * Creates a virtual collection as described by 'vopts'.
 */
Status createVirtualCollection(OperationContext* opCtx,
                               const NamespaceString& ns,
                               const VirtualCollectionOptions& vopts);

/**
 * As above, but only used by replication to apply operations. This allows recreating collections
 * with specific UUIDs (if ui is given). If ui is given and and a collection exists with the same
 * name, the existing collection will be renamed to a temporary name if allowRenameOutOfTheWay is
 * true. This function will invariant if there is an existing collection with the same name and
 * allowRenameOutOfTheWay is false. If ui is not given, an existing collection will result in an
 * error.
 */
Status createCollectionForApplyOps(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<UUID>& ui,
    const BSONObj& cmdObj,
    bool allowRenameOutOfTheWay,
    const boost::optional<BSONObj>& idIndex = boost::none,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier = boost::none);

/**
 * Updates collection options if collections must be clustered by default.
 */
CollectionOptions translateOptionsIfClusterByDefault(
    const NamespaceString& nss,
    CollectionOptions collectionOptions,
    const boost::optional<BSONObj>& idIndex = boost::none);

}  // namespace mongo
