/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/util/uuid.h"

namespace mongo {
/**
 * Runs a ManageSearchIndex command request against the remote search index management endpoint.
 * Passes the remote command response data back to the caller if the status is OK, otherwise throws
 * if the command failed.
 */
BSONObj getSearchIndexManagerResponse(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& uuid,
                                      const BSONObj& userCmd,
                                      boost::optional<SearchQueryViewSpec> view = boost::none);

/**
 * Runs the given command against the remote search index management server, if the remote host
 * information has been set via 'searchIndexManagementHostAndPort'.
 */
BSONObj runSearchIndexCommand(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& cmdObj,
                              const UUID& collUUID,
                              boost::optional<SearchQueryViewSpec> view = boost::none);
/**
 * Helper function to throw if search index management is not properly configured.
 */
void throwIfNotRunningWithRemoteSearchIndexManagement();

/*
 * Search related operations on timeseries collections may either fail or act as a no-op;
 * so the 'failOnTsColl' flag indicates which behavior is appropriate if the collection
 * ends up being timeseries upon lookup.
 */
StatusWith<std::tuple<UUID, const NamespaceString, boost::optional<SearchQueryViewSpec>>>
retrieveCollectionUUIDAndResolveView(OperationContext* opCtx,
                                     const NamespaceString& currentOperationNss,
                                     bool failOnTsColl = true);

}  // namespace mongo
