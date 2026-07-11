// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/executor/async_rpc_error_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(AsyncRPCErrorInfo);

}  // namespace

std::shared_ptr<const ErrorExtraInfo> AsyncRPCErrorInfo::parse(const BSONObj& obj) {
    return std::make_shared<AsyncRPCErrorInfo>(
        Status(ErrorCodes::BadValue, "RemoteCommandExectionError illegally parsed from bson"));
}

void AsyncRPCErrorInfo::serialize(BSONObjBuilder* bob) const {}

namespace async_rpc {

Status unpackRPCStatus(Status status) {
    invariant(status == ErrorCodes::RemoteCommandExecutionError);
    auto errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    if (errorInfo->isLocal()) {
        return errorInfo->asLocal();
    }
    invariant(errorInfo->isRemote());
    auto remoteError = errorInfo->asRemote();
    Status out = remoteError.getRemoteCommandResult();
    if (out.isOK()) {
        out = remoteError.getRemoteCommandWriteConcernError();
    }
    if (out.isOK()) {
        out = remoteError.getRemoteCommandFirstWriteError();
    }
    return out;
}

Status unpackRPCStatusIgnoringWriteErrors(Status status) {
    invariant(status == ErrorCodes::RemoteCommandExecutionError);
    auto errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    if (errorInfo->isLocal()) {
        return errorInfo->asLocal();
    }
    invariant(errorInfo->isRemote());
    auto remoteError = errorInfo->asRemote();
    Status out = remoteError.getRemoteCommandResult();
    if (out.isOK()) {
        out = remoteError.getRemoteCommandWriteConcernError();
    }
    return out;
}

Status unpackRPCStatusIgnoringWriteConcernAndWriteErrors(Status status) {
    invariant(status == ErrorCodes::RemoteCommandExecutionError);
    auto errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    if (errorInfo->isLocal()) {
        return errorInfo->asLocal();
    }
    invariant(errorInfo->isRemote());
    auto remoteError = errorInfo->asRemote();
    return remoteError.getRemoteCommandResult();
}

}  // namespace async_rpc
}  // namespace mongo
