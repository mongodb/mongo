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


#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/executor/async_request_executor.h"
#include "mongo/util/future.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ClusterBuildInfoExecutor final : public AsyncRequestExecutor {
public:
    ClusterBuildInfoExecutor() : AsyncRequestExecutor("ClusterBuildInfoExecutor") {}

    Status handleRequest(std::shared_ptr<RequestExecutionContext> rec) {
        auto result = rec->getReplyBuilder()->getBodyBuilder();
        VersionInfoInterface::instance().appendBuildInfo(&result);
        return Status::OK();
    }

    static ClusterBuildInfoExecutor* get(ServiceContext* svc);
};

const auto getClusterBuildInfoExecutor =
    ServiceContext::declareDecoration<ClusterBuildInfoExecutor>();
ClusterBuildInfoExecutor* ClusterBuildInfoExecutor::get(ServiceContext* svc) {
    return const_cast<ClusterBuildInfoExecutor*>(&getClusterBuildInfoExecutor(svc));
}

const auto clusterBuildInfoExecutorRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ClusterBuildInfoExecutor",
    [](ServiceContext* ctx) { getClusterBuildInfoExecutor(ctx).start(); },
    [](ServiceContext* ctx) { getClusterBuildInfoExecutor(ctx).stop(); }};

class ClusterCmdBuildInfo : public BasicCommand {
public:
    ClusterCmdBuildInfo() : BasicCommand("buildInfo", "buildinfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedWithSecurityToken() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {}  // No auth required

    std::string help() const final {
        return "get version #, etc.\n"
               "{ buildinfo:1 }";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) final {
        VersionInfoInterface::instance().appendBuildInfo(&result);
        return true;
    }

    Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec, const DatabaseName&) final {
        auto opCtx = rec->getOpCtx();
        return ClusterBuildInfoExecutor::get(opCtx->getServiceContext())->schedule(std::move(rec));
    }

} cmdBuildInfo;

}  // namespace
}  // namespace mongo
