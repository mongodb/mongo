// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Contains the name of the command that uses the cursor.
 */
class CursorInUseInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::CursorInUse;

    CursorInUseInfo(std::string_view commandName) : _commandName(std::string{commandName}) {}

    std::string_view commandName() const {
        return _commandName;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    std::string _commandName;
};

}  // namespace mongo
