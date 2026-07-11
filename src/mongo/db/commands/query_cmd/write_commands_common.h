// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/query_cmd/update_metrics.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

/**
 * Contains common functionality shared between the batch write commands in mongos and mongod.
 */

namespace mongo {
namespace auth {

void checkAuthForInsertCommand(AuthorizationSession* authzSession,
                               bool withDocumentValidationBypass,
                               const write_ops::InsertCommandRequest& op);
void checkAuthForUpdateCommand(AuthorizationSession* authzSession,
                               bool withDocumentValidationBypass,
                               const write_ops::UpdateCommandRequest& op);
void checkAuthForDeleteCommand(AuthorizationSession* authzSession,
                               bool withDocumentValidationBypass,
                               const write_ops::DeleteCommandRequest& op);

}  // namespace auth

void incrementUpdateMetrics(const write_ops::UpdateModification& updateMod,
                            const mongo::NamespaceString& ns,
                            UpdateMetrics& updateMetrics,
                            const boost::optional<std::vector<mongo::BSONObj>>& arrayFilters);

}  // namespace mongo
