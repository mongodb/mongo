// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/extension_factory.h"

namespace sdk = mongo::extension::sdk;

class ExtensionA : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        // Intentionally left blank.
    }
};

class ExtensionB : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        // Intentionally left blank.
    }
};

REGISTER_EXTENSION_WITH_VERSION(ExtensionA, (MONGODB_EXTENSION_API_VERSION))
REGISTER_EXTENSION_WITH_VERSION(ExtensionB, (MONGODB_EXTENSION_API_VERSION))
DEFINE_GET_EXTENSION()
