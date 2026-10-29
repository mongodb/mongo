// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

namespace mongo {

class WiredTigerIndexUtil {
    WiredTigerIndexUtil(const WiredTigerIndexUtil&) = delete;
    WiredTigerIndexUtil& operator=(const WiredTigerIndexUtil&) = delete;

private:
    WiredTigerIndexUtil();

public:
    static bool appendCustomStats(WiredTigerRecoveryUnit&,
                                  BSONObjBuilder* output,
                                  double scale,
                                  const std::string& uri);

    static StatusWith<int64_t> compact(OperationContext* opCtx,
                                       WiredTigerRecoveryUnit& wtRu,
                                       const std::string& uri,
                                       const CompactOptions& options);

    static bool isEmpty(OperationContext* opCtx,
                        WiredTigerRecoveryUnit& wtRu,
                        const std::string& uri,
                        uint64_t tableId);

    static void validateStructure(WiredTigerRecoveryUnit&,
                                  const std::string& uri,
                                  const boost::optional<std::string>& configurationOverride,
                                  IndexValidateResults& results);
};

}  // namespace mongo
