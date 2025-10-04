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

#include "mongo/base/error_extra_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

bool ErrorExtraInfoExample::isParserEnabledForTest = false;
bool OptionalErrorExtraInfoExample::isParserEnabledForTest = false;

void ErrorExtraInfoExample::serialize(BSONObjBuilder* builder) const {
    builder->append("data", data);
}

std::shared_ptr<const ErrorExtraInfo> ErrorExtraInfoExample::parse(const BSONObj& obj) {
    uassert(
        40681, "ErrorCodes::ForTestingErrorExtraInfo is only for testing", isParserEnabledForTest);

    return std::make_shared<ErrorExtraInfoExample>(obj["data"].Int());
}

void OptionalErrorExtraInfoExample::serialize(BSONObjBuilder* builder) const {
    builder->append("data", data);
}

std::shared_ptr<const ErrorExtraInfo> OptionalErrorExtraInfoExample::parse(const BSONObj& obj) {
    uassert(4696200,
            "ErrorCodes::ForTestingOptionalErrorExtraInfo is only for testing",
            isParserEnabledForTest);

    return std::make_shared<OptionalErrorExtraInfoExample>(obj["data"].Int());
}

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ErrorExtraInfoExample);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(OptionalErrorExtraInfoExample);

namespace nested::twice {

bool NestedErrorExtraInfoExample::isParserEnabledForTest = false;

void NestedErrorExtraInfoExample::serialize(BSONObjBuilder* builder) const {
    builder->append("data", data);
}

std::shared_ptr<const ErrorExtraInfo> NestedErrorExtraInfoExample::parse(const BSONObj& obj) {
    uassert(51100,
            "ErrorCodes::ForTestingErrorExtraInfoWithExtraInfoInNamespace is only for testing",
            isParserEnabledForTest);

    return std::make_shared<NestedErrorExtraInfoExample>(obj["data"].Int());
}

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(NestedErrorExtraInfoExample);

}  // namespace nested::twice

}  // namespace mongo
