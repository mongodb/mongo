// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"

namespace sdk = mongo::extension::sdk;

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        const bool extensionAPIVersionValid =
            portal->getHostExtensionsAPIVersion().major == MONGODB_EXTENSION_API_MAJOR_VERSION &&
            portal->getHostExtensionsAPIVersion().minor <= MONGODB_EXTENSION_API_MINOR_VERSION;
        // Unit tests are given maxWireVersion 0 by default, so this will always error.
        const bool wireVersionValid = portal->getHostMongoDBMaxWireVersion() > 0;
        sdk_uassert(10726600,
                    "MongoExtensionHostPortal contains incompatible versions",
                    extensionAPIVersionValid && wireVersionValid);
    }
};

REGISTER_EXTENSION(MyExtension)
DEFINE_GET_EXTENSION()
