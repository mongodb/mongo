// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo {

/**
 * This class wraps an error originally thrown when a transaction participant shard fails when
 * unyielding its resources after processing remote responses. This allows distinguishing between a
 * local error versus a remote error, which is important for transaction machinery to correctly
 * handle the error.
 */
class [[MONGO_MOD_PUBLIC]] TransactionParticipantFailedUnyieldInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::TransactionParticipantFailedUnyield;

    static constexpr std::string_view kOriginalErrorFieldName{"originalError"};
    static constexpr std::string_view kOriginalResponseStatusFieldName{"originalResponseStatus"};

    TransactionParticipantFailedUnyieldInfo(
        const Status& originalError, boost::optional<Status> originalResponseStatus = boost::none)
        : _originalError(originalError), _originalResponseStatus(originalResponseStatus) {}

    /**
     * Returns the original error that was thrown when a transaction participant shard failed to
     * unyield its resources after processing remote responses.
     */
    const auto& getOriginalError() const {
        return _originalError;
    }

    /**
     * Returns the status received from the last executed remote response when a transaction
     * participant shard failed to unyield its resources.
     *
     * The AsyncRequestsSender may replace the last remote response with an unyield error, giving
     * precedence to the unyield error over the original remote response. This can result in the
     * loss of some exceptions that needed to mark the routing information as stale. Using this
     * parameter serves as a workaround to address this issue.
     */
    const auto& getOriginalResponseStatus() const {
        return _originalResponseStatus;
    }

    void serialize(BSONObjBuilder* bob) const final;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    Status _originalError;
    boost::optional<Status> _originalResponseStatus;
};

}  // namespace mongo
