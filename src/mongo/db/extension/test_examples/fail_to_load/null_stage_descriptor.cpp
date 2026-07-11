// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/sdk/extension_factory.h"

namespace sdk = mongo::extension::sdk;

class NullStageExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) {
        portal->registerStageDescriptor(nullptr);
    }
};

REGISTER_EXTENSION(NullStageExtension);
DEFINE_GET_EXTENSION()
