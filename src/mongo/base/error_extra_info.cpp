// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
