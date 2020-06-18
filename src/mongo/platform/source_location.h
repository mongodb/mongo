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

#include <cstdint>
#include <ostream>
#include <string>

#if !defined(_MSC_VER) && !defined(__clang__)  // not windows or clang
#include <experimental/source_location>
#endif  // windows

#include <fmt/format.h>

namespace mongo {

/**
 * A SourceLocation is a constexpr type that captures a source location
 *
 * This class mimics the api signature of C++20 std::source_location
 *
 * It is intended to be constructed with MONGO_SOURCE_LOCATION() below.
 */
class SourceLocation {
public:
    constexpr SourceLocation(uint_least32_t line,
                             uint_least32_t column,
                             const char* file_name,
                             const char* function_name) noexcept
        : _line(line), _column(column), _file_name(file_name), _function_name(function_name) {}

    constexpr uint_least32_t line() const noexcept {
        return _line;
    }

    constexpr uint_least32_t column() const noexcept {
        return _column;
    }

    constexpr const char* file_name() const noexcept {
        return _file_name;
    }

    constexpr const char* function_name() const noexcept {
        return _function_name;
    }

private:
    uint_least32_t _line;
    uint_least32_t _column;  // column will be 0 if there isn't compiler support
    const char* _file_name;
    const char* _function_name;
};

/**
 * SourceLocationHolder is intended for convenient io of SourceLocation
 */
class SourceLocationHolder {
public:
    constexpr SourceLocationHolder(SourceLocation&& loc) noexcept
        : _loc(std::forward<SourceLocation>(loc)) {}

    constexpr uint_least32_t line() const noexcept {
        return _loc.line();
    }

    constexpr uint_least32_t column() const noexcept {
        return _loc.column();
    }

    constexpr const char* file_name() const noexcept {
        return _loc.file_name();
    }

    constexpr const char* function_name() const noexcept {
        return _loc.function_name();
    }

    std::string toString() const {
        using namespace fmt::literals;
        return R"({{fileName:"{}", line:{}, functionName:"{}"}})"_format(
            _loc.file_name(), _loc.line(), _loc.function_name());
    }

    friend std::ostream& operator<<(std::ostream& out, const SourceLocationHolder& context) {
        return out << context.toString();
    }

private:
    SourceLocation _loc;
};

/**
 * MONGO_SOURCE_LOCATION() either:
 * - captures std::experimental::source_location::current()
 * - makes a best effort with various macros and local static constants
 *
 * Since __FUNCSIG__ and __PRETTY_FUNCTION__ aren't defined outside of functions, there is also
 * MONGO_SOURCE_LOCATION_NO_FUNC() for use with a default member initializatizer or constant
 * initialization.
 */
#if defined(_MSC_VER)  // windows

// MSVC does not have any of N4810 yet. (see
// https://developercommunity.visualstudio.com/idea/354069/implement-c-library-fundamentals-ts-v2.html)
#define MONGO_SOURCE_LOCATION() ::mongo::SourceLocation(__LINE__, 0ul, __FILE__, __func__)
#define MONGO_SOURCE_LOCATION_NO_FUNC() ::mongo::SourceLocation(__LINE__, 0ul, __FILE__, "")

#elif defined(__clang__)  // windows -> clang

// Clang got __builtin_FILE et al as of 8.0.1 (see https://reviews.llvm.org/D37035)
#define MONGO_SOURCE_LOCATION() ::mongo::SourceLocation(__LINE__, 0ul, __FILE__, __func__)
#define MONGO_SOURCE_LOCATION_NO_FUNC() ::mongo::SourceLocation(__LINE__, 0ul, __FILE__, "")

#elif defined(__GNUG__)  // clang -> gcc

constexpr auto toSourceLocation(std::experimental::source_location loc) {
    // Note that std::experimental::source_location captures __func__, not __PRETTY_FUNC__
    return SourceLocation(loc.line(), loc.column(), loc.file_name(), loc.function_name());
}

#define MONGO_SOURCE_LOCATION() \
    ::mongo::toSourceLocation(std::experimental::source_location::current())
#define MONGO_SOURCE_LOCATION_NO_FUNC() \
    ::mongo::toSourceLocation(std::experimental::source_location::current())

#else  // gcc -> ?

#error "Unknown compiler, cannot approximate std::source_location"

#endif  // ?

}  // namespace mongo
