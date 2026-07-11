// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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

// ExtensionA's major is higher than any host major; ExtensionB matches a host major but its minor
// exceeds the host's supported minor for that major. Host-side negotiation rejects both in phase 1
// and fires 10615505 (matching major, no compatible minor), so neither adapter is ever
// instantiated in the current host order.
REGISTER_EXTENSION_WITH_VERSION(ExtensionA,
                                (::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION + 1,
                                                            MONGODB_EXTENSION_API_MINOR_VERSION}))
REGISTER_EXTENSION_WITH_VERSION(ExtensionB,
                                (::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION,
                                                            MONGODB_EXTENSION_API_MINOR_VERSION +
                                                                1}))
DEFINE_GET_EXTENSION()
