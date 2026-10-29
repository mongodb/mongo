// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ClusterUMCErrorWithWriteConcernErrorInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ClusterUMCErrorWithWriteConcernError;
    ClusterUMCErrorWithWriteConcernErrorInfo(Status mainError, WriteConcernErrorDetail wce);

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    const Status& getMainStatus() const;
    const WriteConcernErrorDetail& getWriteConcernErrorDetail() const;

private:
    Status _mainError;
    WriteConcernErrorDetail _wcError;
};

}  // namespace mongo
