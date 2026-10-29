// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Start Full Time Data Capture
 */
[[MONGO_MOD_PUBLIC]] void startMongoSFTDC(ServiceContext* serviceContext);

/**
 * Stop Full Time Data Capture
 */
[[MONGO_MOD_PUBLIC]] void stopMongoSFTDC();

}  // namespace mongo
