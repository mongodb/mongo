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

#include "mongo/unittest/matcher.h"

#include <fmt/format.h>
#include <memory>
#include <utility>

#include "mongo/util/pcre.h"

namespace mongo::unittest::match {

using namespace fmt::literals;

struct ContainsRegex::Impl {
    explicit Impl(std::string pat) : re(std::move(pat)) {}
    pcre::Regex re;
};

ContainsRegex::ContainsRegex(std::string pattern)
    : _impl{std::make_shared<Impl>(std::move(pattern))} {}

ContainsRegex::~ContainsRegex() = default;

MatchResult ContainsRegex::match(StringData x) const {
    if (_impl->re.matchView(x))
        return {};
    return MatchResult(false, "");
}

std::string ContainsRegex::describe() const {
    return R"(ContainsRegex("{}"))"_format(_impl->re.pattern());
}

}  // namespace mongo::unittest::match
