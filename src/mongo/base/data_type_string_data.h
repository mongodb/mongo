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

#pragma once

#include <cstddef>
#include <cstring>

#include "mongo/base/data_type.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"

#ifndef MONGO_BASE_DATA_TYPE_H_INCLUDE_HANDSHAKE_
#error "do not include directly. Use mongo/base/data_type.h"
#endif  // MONGO_BASE_DATA_TYPE_H_INCLUDE_HANDSHAKE_

// Provides a DataType::Handler specialization for StringData.

namespace mongo {

template <>
struct DataType::Handler<StringData> {
    // Consumes all available data, producing
    // a `StringData(ptr,length)`.
    static Status load(StringData* sdata,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset);

    // Copies `sdata` fully into the [ptr,ptr+length) range.
    // Does nothing and returns an Overflow status if
    // `sdata` doesn't fit.
    static Status store(
        StringData sdata, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset);

    static StringData defaultConstruct() {
        return StringData();
    }
};

}  // namespace mongo
