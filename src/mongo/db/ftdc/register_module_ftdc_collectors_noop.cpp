// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/register_module_ftdc_collectors.h"

namespace mongo {

// Builds that include the module link its real implementation in place of this stub.
std::vector<std::unique_ptr<FTDCCollectorInterface>> makeModuleFTDCPeriodicMetadataCollectors(
    ServiceContext*) {
    return {};
}

}  // namespace mongo
