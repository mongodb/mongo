// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view.h"
#include "mongo/util/modules.h"

namespace mongo::view_util {
/**
 * Throws if the specified document is not a valid view definition.
 */
[[MONGO_MOD_PUBLIC]] void validateViewDefinitionBSON(OperationContext* opCtx,
                                                     const BSONObj& viewDefinition,
                                                     const DatabaseName& dbName);


/**
 * Outcome of parsing a view definition from BSON. Can be one of three possibilities:
 * * Invalid view with invalid namespace: viewName == boost::none, viewDefinition == non-OK status
 * * Invalid view with valid namespace  : viewName != boost::none, viewDefinition == non-OK status
 * * Valid view                         : viewName != boost::none, viewDefinition == OK value
 */
struct [[MONGO_MOD_PUBLIC]] ParsedViewDefinition {
    boost::optional<NamespaceString> viewName;
    StatusWith<std::shared_ptr<ViewDefinition>> viewDefinition;
};
[[MONGO_MOD_PUBLIC]] ParsedViewDefinition parseViewDefinitionBSON(OperationContext* opCtx,
                                                                  const DatabaseName& dbName,
                                                                  const BSONObj& view);

}  // namespace mongo::view_util
