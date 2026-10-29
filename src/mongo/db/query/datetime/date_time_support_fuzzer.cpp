// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_range_cursor.h"
#include "mongo/db/query/datetime/date_time_support.h"

#include <string>
#include <string_view>


extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) try {
    // Need at least 9 bytes: 1 control byte and 8 bytes for pivot
    if (Size < 9) {
        return 0;
    }

    static const mongo::TimeZoneDatabase timeZoneDatabase{};
    static const std::vector<std::string> timezones = timeZoneDatabase.getTimeZoneStrings();
    static const std::array<boost::optional<std::string>, 6> formats = {
        std::string("%Y-%m-%dT%H:%M:%S.%L"),
        std::string("%Y/%m/%d %H:%M:%S:%L"),
        std::string("%Y-%m-%d %H:%M:%S:%L"),
        std::string("%Y/%m/%d %H %M %S %L"),
        std::string(""),
        boost::none};

    mongo::ConstDataRangeCursor drc(Data, Size);

    // use this to determine index and pivot
    uint8_t controlByte = drc.readAndAdvance<uint8_t>();
    uint64_t pivotByte = drc.readAndAdvance<uint64_t>();
    auto newSize = drc.length();

    const mongo::TimeZone timeZone =
        timeZoneDatabase.getTimeZone(timezones[pivotByte % timezones.size()]);

    std::string str = std::string(drc.data(), newSize);

    boost::optional<std::string> strFormat;

    // 16% of the time, random bytes will be fed into the format arg
    uint8_t index = controlByte % (formats.size() + 1);
    if (index < formats.size()) {
        strFormat = formats[index];
    } else {
        // use a pivot so we can vary str and strFormat lengths
        uint64_t pivot;

        if (newSize == 0) {
            pivot = 0;
        } else {
            pivot = pivotByte % newSize;
        }

        strFormat = str.substr(pivot, newSize - pivot);
        str = str.substr(0, pivot);
    }

    try {
        timeZoneDatabase.fromString(str, timeZone, boost::optional<std::string_view>(strFormat));
    } catch (const mongo::AssertionException&) {
    }

    timeZoneDatabase.getTimeZone(str);

    return 0;

} catch (const mongo::AssertionException&) {
    return 0;
}
