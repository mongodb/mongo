// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {
namespace repl {

/**
 * Authenticates conn using the server's cluster-membership credentials.
 *
 * Returns Status::OK() on successful authentication.
 */
Status replAuthenticate(DBClientBase* conn);

}  // namespace repl
}  // namespace mongo
