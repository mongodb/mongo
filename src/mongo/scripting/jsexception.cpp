// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/jsexception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kCodeFieldName = "code"sv;
constexpr auto kCodeNameFieldName = "codeName"sv;
constexpr auto kOriginalErrorFieldName = "originalError"sv;
constexpr auto kReasonFieldName = "errmsg"sv;
constexpr auto kStackFieldName = "stack"sv;
constexpr auto kExtraAttrFieldName = "extraAttr"sv;

}  // namespace

void JSExceptionInfo::serialize(BSONObjBuilder* builder) const {
    builder->append(kStackFieldName, this->stack);
    builder->append(kExtraAttrFieldName, this->extraAttr);

    {
        BSONObjBuilder originalErrorBuilder(builder->subobjStart(kOriginalErrorFieldName));
        originalErrorBuilder.append(kReasonFieldName, this->originalError.reason());
        originalErrorBuilder.append(kCodeFieldName, this->originalError.code());
        originalErrorBuilder.append(kCodeNameFieldName,
                                    ErrorCodes::errorString(this->originalError.code()));
        if (auto extraInfo = this->originalError.extraInfo()) {
            extraInfo->serialize(&originalErrorBuilder);
        }
    }
}

std::shared_ptr<const ErrorExtraInfo> JSExceptionInfo::parse(const BSONObj& obj) {
    auto stack = obj[kStackFieldName].String();
    auto extraAttr = obj[kExtraAttrFieldName].Obj();

    auto originalErrorObj = obj[kOriginalErrorFieldName].Obj();
    auto code = originalErrorObj[kCodeFieldName].Int();
    auto reason = originalErrorObj[kReasonFieldName].checkAndGetStringData();
    Status status(ErrorCodes::Error(code), reason, originalErrorObj);

    return std::make_shared<JSExceptionInfo>(
        std::move(stack), std::move(status), extraAttr.getOwned());
}

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(JSExceptionInfo);

}  // namespace mongo
