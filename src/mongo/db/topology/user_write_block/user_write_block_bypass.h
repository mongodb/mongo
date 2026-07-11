// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
class [[MONGO_MOD_NEEDS_REPLACEMENT]] WriteBlockBypass {
public:
    static WriteBlockBypass& get(OperationContext* opCtx);

    static constexpr std::string_view fieldName() {
        return "mayBypassWriteBlocking"sv;
    }

    bool isWriteBlockBypassEnabled() const;
    void setFromMetadata(OperationContext* opCtx, boost::optional<bool> val);
    void set(bool bypassEnabled);
    void writeAsMetadata(BSONObjBuilder* builder);

private:
    bool _writeBlockBypassEnabled = false;
};
}  // namespace mongo
