// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/extension_factory.h"

namespace sdk = mongo::extension::sdk;

class MyExtension : public sdk::Extension {
public:
    // The initialization function is empty since the test should never reach initialization.
    void initialize(const sdk::HostPortalHandle& portal) override {}
};

// Major version is one more than the currently-supported version. Host-side negotiation rejects
// in phase 1, so this adapter is never instantiated in the current host order.
REGISTER_EXTENSION_WITH_VERSION(MyExtension,
                                (::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_VERSION.major + 1,
                                                            0}))
DEFINE_GET_EXTENSION()
