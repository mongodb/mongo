/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Common implementation shared between shared and router roles.
 *
 * By default, the buildInfo command requires authentication in order to be run.
 * Using the buildInfoAuthMode server parameter however, this command may be
 * invoked in a pre-auth state to return either the entire reply (allowedPreAuth),
 * or a redacted reply of just version information (versionOnlyIfPreAuth).
 *
 * The Router and Shard specializations determine the contents of a full reply
 * in their respective implementations.
 */
class CmdBuildInfoBase : public BasicCommand {
public:
    CmdBuildInfoBase() : BasicCommand("buildInfo", "buildinfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext* svcCtx) const final {
        return AllowedOnSecondary::kAlways;
    }

    // See class comment.
    bool requiresAuth() const final;

    bool adminOnly() const final {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbname,
                                 const BSONObj& request) const final {
        // See class comment.
        return Status::OK();
    }

    std::string help() const final {
        return "get version #, etc.\n"
               "{ buildinfo:1 }";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbname,
             const BSONObj& request,
             BSONObjBuilder& result) final;

    /**
     * Generates complete buildInfo response for authenticated user (or when configured for
     * preAuth).
     */
    virtual void generateBuildInfo(OperationContext* opCtx, BSONObjBuilder& result) = 0;
};

}  // namespace mongo
