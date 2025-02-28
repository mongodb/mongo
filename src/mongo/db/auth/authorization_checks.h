/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_set.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"


namespace mongo::auth {

// Checks if this connection has the privileges necessary to perform a find operation
// on the supplied namespace identifier.
Status checkAuthForFind(AuthorizationSession* authSession, const NamespaceString& ns, bool hasTerm);

// Checks if this connection has the privileges necessary to perform a getMore operation on
// the identified cursor, supposing that cursor is associated with the supplied namespace
// identifier.
Status checkAuthForGetMore(AuthorizationSession* authSession,
                           const NamespaceString& ns,
                           long long cursorID,
                           bool hasTerm);

// Checks if this connection has the privileges necessary to perform the given update on the
// given namespace.
Status checkAuthForUpdate(AuthorizationSession* authSession,
                          OperationContext* opCtx,
                          const NamespaceString& ns,
                          const BSONObj& query,
                          const write_ops::UpdateModification& update,
                          bool upsert);

// Checks if this connection has the privileges necessary to insert to the given namespace.
Status checkAuthForInsert(AuthorizationSession* authSession,
                          OperationContext* opCtx,
                          const NamespaceString& ns);

// Checks if this connection has the privileges necessary to perform a delete on the given
// namespace.
Status checkAuthForDelete(AuthorizationSession* authSession,
                          OperationContext* opCtx,
                          const NamespaceString& ns,
                          const BSONObj& query);

// Checks if this connection has the privileges necessary to perform a killCursor on
// the identified cursor, supposing that cursor is associated with the supplied namespace
// identifier.
Status checkAuthForKillCursors(AuthorizationSession* authSession,
                               const NamespaceString& cursorNss,
                               UserNameIterator cursorOwner);

// Attempts to get the privileges necessary to run the aggregation pipeline specified in
// 'request' on the namespace 'ns' either directly on mongoD or via mongoS.
StatusWith<PrivilegeVector> getPrivilegesForAggregate(AuthorizationSession* authSession,
                                                      const NamespaceString& ns,
                                                      const AggregateCommandRequest& request,
                                                      bool isMongos);

// Checks if this connection has the privileges necessary to create 'ns' with the options
// supplied in 'cmdObj' either directly on mongoD or via mongoS.
Status checkAuthForCreate(AuthorizationSession* authSession,
                          const CreateCommand& cmd,
                          bool isMongos);

// Checks if this connection has the privileges necessary to modify 'ns' with the options
// supplied in 'cmdObj' either directly on mongoD or via mongoS.
Status checkAuthForCollMod(AuthorizationSession* authSession,
                           const NamespaceString& ns,
                           const BSONObj& cmdObj,
                           bool isMongos);

}  // namespace mongo::auth
