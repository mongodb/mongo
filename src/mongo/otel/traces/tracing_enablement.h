// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/modules.h"

namespace mongo::otel::traces {

[[MONGO_MOD_PUBLIC]] bool isTracingEnabled(OperationContext* opCtx);

}
