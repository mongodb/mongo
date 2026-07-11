// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Represents an error returned from a mongod or a mongos when it is in quiesce mode. The
 * error information defined here includes the remaining time the node has left
 * in quiesce mode.
 */
class ShutdownInProgressQuiesceInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ShutdownInProgress;

    ShutdownInProgressQuiesceInfo(long long remainingQuiesceTimeMillis)
        : _remainingQuiesceTimeMillis(remainingQuiesceTimeMillis) {}

    const auto& getRemainingQuiesceTimeMillis() const {
        return _remainingQuiesceTimeMillis;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    long long _remainingQuiesceTimeMillis = 0;
};

}  // namespace mongo
