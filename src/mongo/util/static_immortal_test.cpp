/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/static_immortal.h"

#include <map>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using Map = std::map<std::string, int>;

int ctors;

struct Indestructible {
    explicit Indestructible() {
        ++ctors;
    }

    ~Indestructible() = delete;  // Compile failure if StaticImmortal tries to kill me

    void use() {}
    void useConst() const {}
};

TEST(StaticImmortalTest, BasicConstructorAndCast) {
    ctors = 0;
    {
        auto&& x = *StaticImmortal<Indestructible>();
        x.use();
    }
    ASSERT_EQ(ctors, 1);
}

TEST(StaticImmortalTest, PointerSyntax) {
    auto&& x = StaticImmortal<Indestructible>();
    x->useConst();
    (*x).useConst();
    x->use();
    (*x).use();

    const auto& cx = x;
    cx->useConst();
    (*cx).useConst();
}

TEST(StaticImmortalTest, StaticDurationIdiom) {
    static auto&& x = StaticImmortal<Indestructible>();
    static_assert(std::is_same_v<decltype(x), StaticImmortal<Indestructible>&&>);
}

TEST(StaticImmortalTest, DeducedValueTypeCopyInit) {
    static const StaticImmortal m = Map{{"hello", 123}, {"bye", 456}};
    ASSERT_EQ(m->find("bye")->second, 456);
}

TEST(StaticImmortalTest, DeducedValueTypeExpression) {
    static const StaticImmortal m = [] { return Map{{"hello", 123}, {"bye", 456}}; }();
    ASSERT_EQ(m->find("bye")->second, 456);
}

TEST(StaticImmortalTest, BraceInit) {
    static const StaticImmortal<Map> m{{{"hello", 123}, {"bye", 456}}};
    ASSERT_EQ(m->find("bye")->second, 456);
}

TEST(StaticImmortalTest, ListInit) {
    static const StaticImmortal<Map> m = {{{"hello", 123}, {"bye", 456}}};
    ASSERT_EQ(m->find("bye")->second, 456);
}

TEST(StaticImmortalTest, EmptyBrace) {
    static const StaticImmortal<Map> empty{};
    ASSERT_TRUE(empty->empty());
}

}  // namespace
}  // namespace mongo
