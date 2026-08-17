// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/register_module_ftdc_collectors.h"

#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"

namespace mongo {

void registerModuleFTDCCollectors(ServiceContext* serviceContext, FTDCController* controller) {
    for (auto&& collector : makeModuleFTDCPeriodicMetadataCollectors(serviceContext)) {
        controller->addPeriodicMetadataCollector(std::move(collector));
    }
}

}  // namespace mongo
