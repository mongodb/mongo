// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/shard_role/ddl/rename_collection_gen.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace rename_collection {

[[MONGO_MOD_PARENT_PRIVATE]] Status checkAuthForRenameCollectionCommand(
    Client* client, const RenameCollectionCommand& request);

}  // namespace rename_collection
}  // namespace mongo
