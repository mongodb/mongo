// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;

DEFAULT_EXEC_STAGE(ListSearchIndexes)
DEFAULT_LOGICAL_STAGE(ListSearchIndexes)
DEFAULT_AST_NODE(ListSearchIndexes)
DEFAULT_PARSE_NODE(ListSearchIndexes)

using ListSearchIndexesStageDescriptor =
    sdk::TestStageDescriptor<"$listSearchIndexes", ListSearchIndexesParseNode>;

class SearchIndexManagementExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ListSearchIndexesStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(SearchIndexManagementExtension)
DEFINE_GET_EXTENSION()
