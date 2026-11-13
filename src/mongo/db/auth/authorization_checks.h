// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>


namespace mongo::auth {

/**
 * A small number of namespaces hold state that the server later replays with internal authority,
 * so merely holding privileges on the namespace itself (for instance via write access to the
 * 'config' database) must not be enough to touch them. Access to such a namespace additionally
 * requires the equivalent actions on a "pseudo namespace" that must be explicitly granted.
 *
 * Returns the privilege that must also be held in order to take 'actions' on 'resource', or
 * boost::none if 'resource' is not restricted.
 *
 * Callers that build a PrivilegeVector should prefer addImplicitPrivilegesForRestrictedNamespaces.
 *
 * isRestrictedNamespace answers only whether a namespace is one of these. It is for the rarer case
 * of a command whose authorization rules offer a shortcut that must not apply to such a namespace;
 * anything checking privileges should use one of the two helpers instead.
 */
[[MONGO_MOD_PUBLIC]] bool isRestrictedNamespace(const NamespaceString& ns);

[[MONGO_MOD_PUBLIC]] boost::optional<Privilege> getImplicitPrivilegeForRestrictedNamespace(
    const ResourcePattern& resource, const ActionSet& actions);

/**
 * For every restricted namespace named by a privilege already in 'privileges', appends the
 * implicit privilege required to act on it with that privilege's actions. Callers must invoke this
 * after they have finished populating 'privileges' and before checking them.
 */
[[MONGO_MOD_PUBLIC]] void addImplicitPrivilegesForRestrictedNamespaces(PrivilegeVector* privileges);

// Checks if this connection has the privileges necessary to perform a find operation
// on the supplied namespace identifier.
[[MONGO_MOD_PUBLIC]] Status checkAuthForFind(AuthorizationSession* authSession,
                                             const NamespaceString& ns,
                                             bool hasTerm);

// Checks if this connection has the privileges necessary to perform a getMore operation on
// the identified cursor, supposing that cursor is associated with the supplied namespace
// identifier.
[[MONGO_MOD_PUBLIC]] Status checkAuthForGetMore(AuthorizationSession* authSession,
                                                const NamespaceString& ns,
                                                long long cursorID,
                                                bool hasTerm);

// Checks if this connection has the privileges necessary to perform the given update on the
// given namespace.
[[MONGO_MOD_PUBLIC]] Status checkAuthForUpdate(AuthorizationSession* authSession,
                                               OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               const BSONObj& query,
                                               const write_ops::UpdateModification& update,
                                               bool upsert);

// Checks if this connection has the privileges necessary to insert to the given namespace.
[[MONGO_MOD_PUBLIC]] Status checkAuthForInsert(AuthorizationSession* authSession,
                                               OperationContext* opCtx,
                                               const NamespaceString& ns);

// Checks if this connection has the privileges necessary to perform a delete on the given
// namespace.
[[MONGO_MOD_PUBLIC]] Status checkAuthForDelete(AuthorizationSession* authSession,
                                               OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               const BSONObj& query);

// Checks if this connection has the privileges necessary to perform a killCursor on
// the identified cursor, supposing that cursor is associated with the supplied namespace
// identifier.
[[MONGO_MOD_PUBLIC]] Status checkAuthForKillCursors(AuthorizationSession* authSession,
                                                    const NamespaceString& cursorNss,
                                                    const boost::optional<UserName>& cursorOwner);

// Checks if this connection has the privileges necessary to perform a releaseMemory on
// the identified cursor, supposing that cursor is associated with the supplied namespace
// identifier.
[[MONGO_MOD_PUBLIC]] Status checkAuthForReleaseMemory(AuthorizationSession* authSession,
                                                      const NamespaceString& cursorNss);

// Attempts to get the privileges necessary to run the aggregation pipeline specified in
// 'request' on the namespace 'ns' either directly on mongoD or via mongoS.
[[MONGO_MOD_PUBLIC]] StatusWith<PrivilegeVector> getPrivilegesForAggregate(
    OperationContext* opCtx,
    AuthorizationSession* authSession,
    const NamespaceString& ns,
    const AggregateCommandRequest& request,
    bool isMongos);

// Checks if this connection has the privileges necessary to create 'ns' with the options
// supplied in 'cmdObj' either directly on mongoD or via mongoS.
[[MONGO_MOD_PUBLIC]] Status checkAuthForCreate(OperationContext* opCtx,
                                               AuthorizationSession* authSession,
                                               const CreateCommand& cmd,
                                               bool isMongos);

// Checks if this connection has the privileges necessary to modify 'ns' with the options
// supplied in 'cmdObj' either directly on mongoD or via mongoS.
[[MONGO_MOD_PUBLIC]] Status checkAuthForCollMod(OperationContext* opCtx,
                                                AuthorizationSession* authSession,
                                                const NamespaceString& ns,
                                                const BSONObj& cmdObj,
                                                bool isMongos,
                                                const SerializationContext& serializationContext);

}  // namespace mongo::auth
