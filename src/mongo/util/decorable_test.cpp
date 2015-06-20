/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/utility.hpp>

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/decoration_container.h"
#include "mongo/util/decoration_registry.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

static int numConstructedAs;
static int numDestructedAs;

class A {
public:
    A() : value(0) {
        ++numConstructedAs;
    }
    ~A() {
        ++numDestructedAs;
    }
    int value;
};

class ThrowA {
public:
    ThrowA() : value(0) {
        uasserted(ErrorCodes::Unauthorized, "Throwing in a constructor");
    }

    int value;
};

TEST(DecorableTest, DecorableType) {
    class MyDecorable : public Decorable<MyDecorable> {};
    const auto dd1 = MyDecorable::declareDecoration<A>();
    const auto dd2 = MyDecorable::declareDecoration<A>();
    const auto dd3 = MyDecorable::declareDecoration<int>();
    numConstructedAs = 0;
    numDestructedAs = 0;
    {
        MyDecorable decorable1;
        ASSERT_EQ(2, numConstructedAs);
        ASSERT_EQ(0, numDestructedAs);
        MyDecorable decorable2;
        ASSERT_EQ(4, numConstructedAs);
        ASSERT_EQ(0, numDestructedAs);

        ASSERT_EQ(0, dd1(decorable1).value);
        ASSERT_EQ(0, dd2(decorable1).value);
        ASSERT_EQ(0, dd1(decorable2).value);
        ASSERT_EQ(0, dd2(decorable2).value);
        ASSERT_EQ(0, dd3(decorable2));
        dd1(decorable1).value = 1;
        dd2(decorable1).value = 2;
        dd1(decorable2).value = 3;
        dd2(decorable2).value = 4;
        dd3(decorable2) = 5;
        ASSERT_EQ(1, dd1(decorable1).value);
        ASSERT_EQ(2, dd2(decorable1).value);
        ASSERT_EQ(3, dd1(decorable2).value);
        ASSERT_EQ(4, dd2(decorable2).value);
        ASSERT_EQ(5, dd3(decorable2));
    }
    ASSERT_EQ(4, numDestructedAs);
}

TEST(DecorableTest, SimpleDecoration) {
    numConstructedAs = 0;
    numDestructedAs = 0;
    DecorationRegistry registry;
    const auto dd1 = registry.declareDecoration<A>();
    const auto dd2 = registry.declareDecoration<A>();
    const auto dd3 = registry.declareDecoration<int>();

    {
        DecorationContainer decorable1(&registry);
        ASSERT_EQ(2, numConstructedAs);
        ASSERT_EQ(0, numDestructedAs);
        DecorationContainer decorable2(&registry);
        ASSERT_EQ(4, numConstructedAs);
        ASSERT_EQ(0, numDestructedAs);

        ASSERT_EQ(0, decorable1.getDecoration(dd1).value);
        ASSERT_EQ(0, decorable1.getDecoration(dd2).value);
        ASSERT_EQ(0, decorable2.getDecoration(dd1).value);
        ASSERT_EQ(0, decorable2.getDecoration(dd2).value);
        ASSERT_EQ(0, decorable2.getDecoration(dd3));
        decorable1.getDecoration(dd1).value = 1;
        decorable1.getDecoration(dd2).value = 2;
        decorable2.getDecoration(dd1).value = 3;
        decorable2.getDecoration(dd2).value = 4;
        decorable2.getDecoration(dd3) = 5;
        ASSERT_EQ(1, decorable1.getDecoration(dd1).value);
        ASSERT_EQ(2, decorable1.getDecoration(dd2).value);
        ASSERT_EQ(3, decorable2.getDecoration(dd1).value);
        ASSERT_EQ(4, decorable2.getDecoration(dd2).value);
        ASSERT_EQ(5, decorable2.getDecoration(dd3));
    }
    ASSERT_EQ(4, numDestructedAs);
}

TEST(DecorableTest, ThrowingConstructor) {
    numConstructedAs = 0;
    numDestructedAs = 0;

    DecorationRegistry registry;
    registry.declareDecoration<A>();
    registry.declareDecoration<ThrowA>();
    registry.declareDecoration<A>();

    try {
        DecorationContainer d(&registry);
    } catch (const UserException& ex) {
        ASSERT_EQ(ErrorCodes::Unauthorized, ex.getCode());
    }
    ASSERT_EQ(1, numConstructedAs);
    ASSERT_EQ(1, numDestructedAs);
}

TEST(DecorableTest, Alignment) {
    DecorationRegistry registry;
    const auto firstChar = registry.declareDecoration<char>();
    const auto firstInt = registry.declareDecoration<int>();
    const auto secondChar = registry.declareDecoration<int>();
    const auto secondInt = registry.declareDecoration<int>();
    DecorationContainer d(&registry);
    ASSERT_EQ(0U,
              reinterpret_cast<uintptr_t>(&d.getDecoration(firstChar)) %
                  std::alignment_of<char>::value);
    ASSERT_EQ(0U,
              reinterpret_cast<uintptr_t>(&d.getDecoration(secondChar)) %
                  std::alignment_of<char>::value);
    ASSERT_EQ(0U,
              reinterpret_cast<uintptr_t>(&d.getDecoration(firstInt)) %
                  std::alignment_of<int>::value);
    ASSERT_EQ(0U,
              reinterpret_cast<uintptr_t>(&d.getDecoration(secondInt)) %
                  std::alignment_of<int>::value);
}

}  // namespace
}  // namespace mongo
