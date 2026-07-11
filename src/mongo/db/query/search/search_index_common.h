// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/util/modules.h"
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
