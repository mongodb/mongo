// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/db/query/query_knobs/query_knob_test_gen.h"
#include "mongo/idl/idl_parser.h"

#include <string_view>

namespace mongo {

Status validateTestIntKnobCallback(const int& value, const boost::optional<TenantId>&) {
    if (value == 13) {
        return Status(ErrorCodes::BadValue, "13 is a forbidden sentinel value for testing");
    }
    return Status::OK();
}

namespace test_knobs {
REGISTER_QUERY_KNOBS(TestKnobs, MONGO_EXPAND_QUERY_KNOBS_TEST)
}  // namespace test_knobs

void TestEnumKnob::append(OperationContext*,
                          BSONObjBuilder* b,
                          std::string_view name,
                          const boost::optional<TenantId>&) {
    *b << name << idl::serialize(_data.get());
}

Status TestEnumKnob::setFromString(std::string_view value, const boost::optional<TenantId>&) {
    _data = idl::deserialize<TestKnobModeEnum>(value, IDLParserContext("testEnumKnob"));
    return Status::OK();
}

}  // namespace mongo
