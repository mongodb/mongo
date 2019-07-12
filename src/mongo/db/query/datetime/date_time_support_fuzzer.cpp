/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <string>

#include "boost/none_t.hpp"

#include "mongo/base/data_range_cursor.h"
#include "mongo/db/query/datetime/date_time_support.h"


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
        timeZoneDatabase.fromString(str, timeZone, boost::optional<mongo::StringData>(strFormat));
    } catch (const mongo::AssertionException&) {
    }

    timeZoneDatabase.getTimeZone(str);

    return 0;

} catch (const mongo::AssertionException&) {
    return 0;
}
