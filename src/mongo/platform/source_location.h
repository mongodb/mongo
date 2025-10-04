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

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <version>

#include <fmt/format.h>

#if __cpp_lib_source_location >= 201907L
#include <source_location>
#define MONGO_SOURCE_LOCATION_HAVE_STD
#endif

namespace MONGO_MOD_PUB mongo {

struct SourceLocationFormatter {
    constexpr auto parse(auto&& ctx) const {
        return ctx.begin();
    }

    /**
     * Normally produces `{file}:{line}:{column}:{func}`.
     * If `line==0`, produces `(unknown location)` instead.
     * The column and function names are conditionally present.
     * If `column==0`, then `:{column}` is omitted.
     * If `func` is empty, then `:{func}` is omitted.
     */
    auto format(const auto& loc, auto& ctx) const {
        auto out = ctx.out();
        auto ln = loc.line();
        if (!ln)
            return fmt::format_to(std::move(out), "(unknown location)");
        out = fmt::format_to(out, "{}:{}", loc.file_name(), ln);
        if (auto c = loc.column())
            out = fmt::format_to(std::move(out), ":{}", c);
        if (auto f = loc.function_name(); f && *f)
            out = fmt::format_to(std::move(out), ":{}", f);
        return out;
    }
};

#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
/**
 * Wraps and emulates the API of `std::source_location`.
 * `std::source_location` is only designed for exposing compiler
 * intrinsics, and users must use alternative types if they want more
 * features than that.
 */
class WrappedStdSourceLocation {
public:
    constexpr WrappedStdSourceLocation() noexcept = default;

    explicit(false) constexpr WrappedStdSourceLocation(std::source_location loc) noexcept
        : _loc{loc} {}

    constexpr const char* file_name() const noexcept {
        return _loc.file_name();
    }

    constexpr uint_least32_t line() const noexcept {
        return _loc.line();
    }

    constexpr const char* function_name() const noexcept {
        return _loc.function_name();
    }

    constexpr uint_least32_t column() const noexcept {
        return _loc.column();
    }

    friend std::string toString(WrappedStdSourceLocation loc) {
        return loc._toString();
    }

    friend std::ostream& operator<<(std::ostream& os, WrappedStdSourceLocation loc) {
        return os << toString(loc);
    }

private:
    std::string _toString() const;

    std::source_location _loc;
};

}  // namespace mongo

template <>
struct MONGO_MOD_PUB fmt::formatter<mongo::WrappedStdSourceLocation>
    : mongo::SourceLocationFormatter {};

namespace MONGO_MOD_PUB mongo {

/** Must appear after formatter specialization. */
inline std::string WrappedStdSourceLocation::_toString() const {
    return fmt::format("{}", *this);
}

using SourceLocation = WrappedStdSourceLocation;

/** Fast. Loads a single packed struct address. */
#define MONGO_SOURCE_LOCATION() ::mongo::WrappedStdSourceLocation(std::source_location::current())

#endif  // MONGO_SOURCE_LOCATION_HAVE_STD

/**
 * Emulates the API of `std::source_location`, but can be
 * constructed from arguments.
 * Used only where `WrappedStdSourceLocation` isn't available or won't work.
 */
class SyntheticSourceLocation {
public:
    constexpr SyntheticSourceLocation() noexcept = default;

    constexpr SyntheticSourceLocation(const char* file,
                                      uint_least32_t line,
                                      const char* function = "",
                                      uint_least32_t column = 0) noexcept
        : _file_name{file}, _function_name{function}, _line{line}, _column{column} {}

    constexpr const char* file_name() const noexcept {
        return _file_name;
    }
    constexpr uint_least32_t line() const noexcept {
        return _line;
    }
    constexpr const char* function_name() const noexcept {
        return _function_name;
    }
    constexpr uint_least32_t column() const noexcept {
        return _column;
    }

    friend std::string toString(SyntheticSourceLocation loc) {
        return loc._toString();
    }

    friend std::ostream& operator<<(std::ostream& os, SyntheticSourceLocation loc) {
        return os << toString(loc);
    }

private:
    std::string _toString() const;

    const char* _file_name = "";
    const char* _function_name = "";
    uint_least32_t _line = 0;
    uint_least32_t _column = 0;
};

}  // namespace mongo

template <>
struct MONGO_MOD_PUB fmt::formatter<mongo::SyntheticSourceLocation>
    : mongo::SourceLocationFormatter {};

namespace MONGO_MOD_PUB mongo {

/** Must appear after formatter specialization. */
inline std::string SyntheticSourceLocation::_toString() const {
    return fmt::format("{}", *this);
}

#ifndef MONGO_SOURCE_LOCATION_HAVE_STD

using SourceLocation = SyntheticSourceLocation;

/**
 * If we don't have `std::source_location` (toolchain-v4's clang), we can still
 * make a synthetic.
 * Old clang is missing std::source_location but has all the builtins.
 */
constexpr SyntheticSourceLocation currentSyntheticSourceLocation(
    const char* file = __builtin_FILE(),
    int line = __builtin_LINE(),
    const char* func = __builtin_FUNCTION(),
    int col = __builtin_COLUMN()) {
    return SyntheticSourceLocation(file, line, func, col);
}

#define MONGO_SOURCE_LOCATION() ::mongo::currentSyntheticSourceLocation()

#endif  // no std::source_location

/** Provided only for use in the source_location_test. */
constexpr SourceLocation makeHeaderSourceLocation_forTest() {
    return MONGO_SOURCE_LOCATION();
}

}  // namespace MONGO_MOD_PUB mongo
