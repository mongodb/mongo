// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/ftdc/collector.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo {

class FTDCController;
class ServiceContext;

/**
 * Builds the FTDC periodic metadata collectors provided by a module that is not compiled into
 * every build. Builds without the module link a stub that returns no collectors.
 */
[[MONGO_MOD_PUBLIC]] std::vector<std::unique_ptr<FTDCCollectorInterface>>
makeModuleFTDCPeriodicMetadataCollectors(ServiceContext* serviceContext);

/**
 * Registers the module's collectors with the controller. FTDCController is private to this module,
 * so the controller handoff happens here rather than in the module's implementation.
 */
void registerModuleFTDCCollectors(ServiceContext* serviceContext, FTDCController* controller);

}  // namespace mongo
