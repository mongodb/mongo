/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

namespace mongo::unittest {
/**
 * Represents data about a single unit test.
 */
class TestInfo {
public:
    TestInfo(StringData suiteName, StringData testName, StringData file, unsigned int line)
        : _suiteName(suiteName), _testName(testName), _file(file), _line(line) {}

    StringData suiteName() const {
        return _suiteName;
    }
    StringData testName() const {
        return _testName;
    }
    StringData file() const {
        return _file;
    }
    unsigned int line() const {
        return _line;
    }

private:
    StringData _suiteName;
    StringData _testName;
    StringData _file;
    unsigned int _line;
};
}  // namespace mongo::unittest
