// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/static_immortal.h"

#include "mongo/unittest/unittest.h"

#include <map>
#include <string>
#include <type_traits>
#include <utility>

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
    static const StaticImmortal m = [] {
        return Map{{"hello", 123}, {"bye", 456}};
    }();
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
