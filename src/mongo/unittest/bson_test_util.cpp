// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <ostream>

namespace mongo {
namespace unittest {
using namespace std::literals::string_view_literals;

std::string formatJsonStr(const std::string& input) {
    BSONObj obj = fromjson(input);
    const static JsonStringFormat format = JsonStringFormat::ExtendedRelaxedV2_0_0;
    std::string str = obj.jsonString(format);

    // Raw JSON strings additionally have `fromjson(R())`, so subtract that from the auto-update max
    // line length.
    static constexpr size_t kRawJsonMaxLineLength =
        kAutoUpdateMaxLineLength - "fromjson(R())"sv.size();
    if (str.size() > kRawJsonMaxLineLength) {
        str = obj.jsonString(format, 1);
    }
    return str::stream() << "R\"(" << str << ")\"";
}

}  // namespace unittest
}  // namespace mongo
