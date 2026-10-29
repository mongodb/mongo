// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/extension_factory.h"

namespace sdk = mongo::extension::sdk;

class MyExtension : public sdk::Extension {
public:
    // The initialization function is empty since the test should never reach initialization.
    void initialize(const sdk::HostPortalHandle& portal) override {}
};

// No definition of either get_mongodb_extension_versions or get_mongodb_extension), which is
// intentional to simulate a malformed extension missing the entire load protocol.
