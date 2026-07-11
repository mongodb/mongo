// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class WiredTigerKVEngine;

/**
 * Adds "wiredTiger" to the results of db.serverStatus().
 */
class WiredTigerServerStatusSection : public ServerStatusSection {
public:
    static constexpr std::string_view kServerStatusSectionName = "wiredTiger"sv;

    using ServerStatusSection::ServerStatusSection;
    bool includeByDefault() const override;
    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override;
};

}  // namespace mongo
