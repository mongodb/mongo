// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] RemoveShardDrainingInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::RemoveShardDrainingInProgress;

    RemoveShardDrainingInfo(RemoveShardProgress progress) : _progress(progress) {}

    const RemoveShardProgress& getProgress() const {
        return _progress;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    RemoveShardProgress _progress;
};
}  // namespace mongo
