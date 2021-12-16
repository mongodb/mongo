/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/optional.hpp>

namespace mongo::sorter {
struct Options {
    // The number of KV pairs to be returned. 0 indicates no limit.
    unsigned long long limit = 0;

    // When in-memory memory usage exceeds this value, we try to spill to disk. This is approximate.
    size_t maxMemoryUsageBytes = 64 * 1024 * 1024;

    // Whether we are allowed to spill to disk. If this is none and in-memory exceeds
    // maxMemoryUsageBytes, we will uassert.
    boost::optional<std::string> tempDir;

    // In case the sorter spills encrypted data to disk that must be readable even after process
    // restarts, it must encrypt with a persistent key. This key is accessed using the database
    // name that the sorted collection lives in. If encryption is enabled and dbName is boost::none,
    // a temporary key is used.
    boost::optional<std::string> dbName;
};
}  // namespace mongo::sorter
