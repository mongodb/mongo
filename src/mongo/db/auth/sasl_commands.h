// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/sasl_commands_gen.h"
#include "mongo/util/modules.h"

namespace mongo {
class OperationContext;

namespace auth {
[[MONGO_MOD_PUBLIC]] SaslReply runSaslStart(OperationContext* opCtx,
                                            AuthenticationSession* session,
                                            const SaslStartCommand& request);


}  // namespace auth

/**
 * Handle hello: { speculativeAuthenticate: {...} }
 */
[[MONGO_MOD_PUBLIC]] void doSpeculativeSaslStart(OperationContext* opCtx,
                                                 const BSONObj& sourceObj,
                                                 BSONObjBuilder* result);
}  // namespace mongo
