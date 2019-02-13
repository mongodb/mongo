/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/scripting/jsexception.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

constexpr auto kCodeFieldName = "code"_sd;
constexpr auto kCodeNameFieldName = "codeName"_sd;
constexpr auto kOriginalErrorFieldName = "originalError"_sd;
constexpr auto kReasonFieldName = "errmsg"_sd;
constexpr auto kStackFieldName = "stack"_sd;

}  // namespace

void JSExceptionInfo::serialize(BSONObjBuilder* builder) const {
    builder->append(kStackFieldName, this->stack);

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

    auto originalErrorObj = obj[kOriginalErrorFieldName].Obj();
    auto code = originalErrorObj[kCodeFieldName].Int();
    auto reason = originalErrorObj[kReasonFieldName].checkAndGetStringData();
    Status status(ErrorCodes::Error(code), reason, originalErrorObj);

    return std::make_shared<JSExceptionInfo>(std::move(stack), std::move(status));
}

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(JSExceptionInfo);

}  // namespace mongo
