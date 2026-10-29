// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/database_name.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>
#include <tuple>

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class OperationContext;

/**
 * Utilities for dealing with what used to be called metadata.
 */
namespace [[MONGO_MOD_PUBLIC]] rpc {
class ImpersonatedClientSessionGuard;
/**
 * Returns an empty metadata object.
 */
BSONObj makeEmptyMetadata();

/**
 * Reads metadata from a metadata object and sets it on this OperationContext.
 */
void readRequestMetadata(OperationContext* opCtx,
                         const GenericArguments& requestArgs,
                         bool cmdRequiresAuth,
                         boost::optional<ImpersonatedClientSessionGuard>& clientSessionGuard);

/**
 * Installs the IFRContext on `opCtx` from a request's generic arguments (`ifrFlags` /
 * `ifrSenderVersion`). Idempotent: if a context is already installed (e.g. a command installed
 * one during parse), this is a no-op that preserves the existing shared_ptr. Safe to call before
 * command->parse so that lite-parse and any IFR flag reads during parsing observe wire values,
 * not local defaults. Enforces that only internally-authorized senders may propagate ifrFlags.
 */
void installIfrContextFromWire(OperationContext* opCtx, const GenericArguments& requestArgs);

/**
 * A legacy command object and a corresponding query flags bitfield. The legacy command object
 * may contain metadata fields, so it cannot safely be passed to a command's run method.
 */
using LegacyCommandAndFlags = std::tuple<BSONObj, int>;

/**
 * Upconverts a legacy command request into an OpMessageRequest.
 */
OpMsgRequest upconvertRequest(const DatabaseName& dbName,
                              BSONObj legacyCmdObj,
                              int queryFlags,
                              boost::optional<auth::ValidatedTenancyScope> vts = boost::none);


/**
 * A function type for writing request metadata. The function takes a pointer to an optional
 * OperationContext so metadata associated with a Client can be appended, a pointer to a
 * BSONObjBuilder used to construct the metadata object and returns a Status indicating if the
 * metadata was written successfully.
 */
using RequestMetadataWriter =
    std::function<Status(OperationContext* opCtx, BSONObjBuilder* metadataBuilder)>;

/**
 * A function type for reading reply metadata. The function takes a a reference to a
 * metadata object received in a command reply and the server address of the
 * host that executed the command and returns a Status indicating if the
 * metadata was read successfully.
 *
 * TODO: would it be a layering violation if this hook took an OperationContext* ?
 */
using ReplyMetadataReader = std::function<Status(
    OperationContext* opCtx, const BSONObj& replyMetadata, std::string_view sourceHost)>;

}  // namespace rpc
}  // namespace mongo
