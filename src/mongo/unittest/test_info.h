// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::unittest {
/**
 * Represents data about a single unit test.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] TestInfo {
public:
    constexpr TestInfo(std::string_view suiteName,
                       std::string_view testName,
                       std::string_view file,
                       unsigned int line,
                       const std::type_info* baseTypeInfo = nullptr)
        : _suiteName(suiteName),
          _testName(testName),
          _file(file),
          _line(line),
          _baseTypeInfo(baseTypeInfo) {}

    constexpr std::string_view suiteName() const {
        return _suiteName;
    }
    constexpr std::string_view testName() const {
        return _testName;
    }
    constexpr std::string_view file() const {
        return _file;
    }
    constexpr unsigned int line() const {
        return _line;
    }
    constexpr const std::type_info* baseTypeInfo() const {
        return _baseTypeInfo;
    }

private:
    std::string_view _suiteName;
    std::string_view _testName;
    std::string_view _file;
    unsigned int _line;
    const std::type_info* _baseTypeInfo{};
};
}  // namespace mongo::unittest
