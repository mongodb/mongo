// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace mongo {

[[MONGO_MOD_PUBLIC]] void doSpeculativeAuthenticate(OperationContext* opCtx,
                                                    BSONObj helloCmd,
                                                    BSONObjBuilder* result);

}  // namespace mongo
