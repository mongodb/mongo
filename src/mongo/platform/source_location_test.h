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

#pragma once

#include "mongo/unittest/unittest.h"

#include "mongo/platform/source_location.h"

namespace mongo {
inline bool operator==(const SourceLocationHolder& lhs, const SourceLocationHolder& rhs) {
    return lhs.line() == rhs.line()            //
        && lhs.column() == rhs.column()        //
        && lhs.file_name() == rhs.file_name()  //
        && lhs.function_name() == rhs.function_name();
}

inline bool operator!=(const SourceLocationHolder& lhs, const SourceLocationHolder& rhs) {
    return !(lhs == rhs);
}

// Simple recursive constexpr string comparison to play nice with static_assert
constexpr bool areEqual(const char* string1, const char* string2) {
    return (string1 != nullptr)  //
        && (string2 != nullptr)  //
        && *string1 == *string2  //
        && (*string1 == '\0' || areEqual(string1 + 1, string2 + 1));
}

inline constexpr SourceLocation makeHeaderSourceLocationForTest() {
    return MONGO_SOURCE_LOCATION();
}
}  // namespace mongo
