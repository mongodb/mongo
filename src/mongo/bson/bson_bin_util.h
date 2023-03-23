/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/builder.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace mongo {
namespace bson_bin_util {

std::string toHex(const char* data, std::size_t len) {
    auto rawString = mongo::StringData(data, len);
    std::ostringstream hexString;
    hexString << std::hex << std::uppercase;
    for (std::size_t i = 0; rawString.size() > i; ++i) {
        hexString << "x" << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(static_cast<unsigned char>(rawString[i])) << " ";
    }
    return hexString.str();
}

std::string toHex(const BufBuilder& bldr) {
    return toHex(static_cast<const char*>(bldr.buf()), static_cast<std::size_t>(bldr.len()));
}

std::string toHex(const BSONBinData& binData) {
    return toHex(static_cast<const char*>(binData.data), static_cast<std::size_t>(binData.length));
}

std::string toHex(const BSONObj& obj) {
    return toHex(obj.objdata(), static_cast<std::size_t>(obj.objsize()));
}

}  // namespace bson_bin_util
}  // namespace mongo
