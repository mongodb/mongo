// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/server_status/server_status.h"

namespace mongo::transport {

class Connections : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override;

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override;
};

}  // namespace mongo::transport
