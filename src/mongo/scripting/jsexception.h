// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {

/**
 * Represents information about a JavaScript exception. The "message" is stored as the Status's
 * reason(), so only the "stack" is stored here.
 *
 * This class wraps an existing error and serializes it in a lossless way, so any other metadata
 * about the JavaScript exception is also preserved.
 */
class [[MONGO_MOD_PUBLIC]] JSExceptionInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::JSInterpreterFailureWithStack;

    void serialize(BSONObjBuilder*) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    explicit JSExceptionInfo(std::string stack_, Status originalError_, BSONObj extraAttr_ = {})
        : stack(std::move(stack_)),
          originalError(std::move(originalError_)),
          extraAttr(std::move(extraAttr_)) {
        invariant(!stack.empty());
        invariant(!originalError.isOK());
    }

    const std::string stack;
    const Status originalError;
    // Stores optional extra attributes of an error.
    const BSONObj extraAttr;
};

}  // namespace mongo
