/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

namespace {

using namespace fmt::literals;

class DecorableTest : public unittest::Test {
public:
    struct Stats {
        int constructed;
        int copyConstructed;
        int copyAssigned;
        int destructed;
    };

    class A {
    public:
        A() {
            ++stats.constructed;
        }
        A(const A& other) : value(other.value) {
            ++stats.copyConstructed;
        }
        A& operator=(const A& rhs) {
            value = rhs.value;
            ++stats.copyAssigned;
            return *this;
        }
        ~A() {
            ++stats.destructed;
        }

        int value = 0;
    };

    void setUp() override {
        stats = {};
    }

    static inline Stats stats{};
};


TEST_F(DecorableTest, SimpleDecoration) {
    struct X : Decorable<X> {};
    static auto da1 = X::declareDecoration<A>();
    static auto da2 = X::declareDecoration<A>();
    static auto di = X::declareDecoration<int>();

    {
        X x1;
        ASSERT_EQ(stats.constructed, 2);
        ASSERT_EQ(stats.destructed, 0);
        X x2;
        ASSERT_EQ(stats.constructed, 4);
        ASSERT_EQ(stats.destructed, 0);

        // Check for zero-init
        ASSERT_EQ(x1[da1].value, 0);
        ASSERT_EQ(x1[da2].value, 0);
        ASSERT_EQ(x2[di], 0);
        ASSERT_EQ(x2[da1].value, 0);
        ASSERT_EQ(x2[da2].value, 0);
        ASSERT_EQ(x2[di], 0);

        // Check for crosstalk among decorations.
        x1[da1].value = 1;
        x1[da2].value = 2;
        x1[di] = 3;
        x2[da1].value = 4;
        x2[da2].value = 5;
        x2[di] = 6;
        ASSERT_EQ(x1[da1].value, 1);
        ASSERT_EQ(x1[da2].value, 2);
        ASSERT_EQ(x1[di], 3);
        ASSERT_EQ(x2[da1].value, 4);
        ASSERT_EQ(x2[da2].value, 5);
        ASSERT_EQ(x2[di], 6);
    }
    ASSERT_EQ(stats.destructed, 4);
}

TEST_F(DecorableTest, ThrowingConstructor) {
    struct Thrower {
        Thrower() {
            iasserted(ErrorCodes::Unauthorized, "Throwing in a constructor");
        }
    };
    struct X : Decorable<X> {};
    static std::tuple d{
        X::declareDecoration<A>(),
        X::declareDecoration<Thrower>(),
        X::declareDecoration<A>(),
    };

    ASSERT_THROWS(X{}, ExceptionFor<ErrorCodes::Unauthorized>);
    ASSERT_EQ(stats.constructed, 1);
    ASSERT_EQ(stats.destructed, 1);
}

TEST_F(DecorableTest, Alignment) {
    struct X : Decorable<X> {};
    static std::tuple d{
        X::declareDecoration<char>(),
        X::declareDecoration<int>(),
        X::declareDecoration<char>(),
        X::declareDecoration<int>(),
    };

    X x;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&x[get<1>(d)]) % alignof(int), 0);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&x[get<3>(d)]) % alignof(int), 0);
}

// Verify that decorations with alignment requirements > alignof(std::max_align_t) have their
// alignment requirement respected.
TEST_F(DecorableTest, ExtendedAlignment) {
    struct X : Decorable<X> {};
    constexpr std::size_t overAlignedRequirement = alignof(std::max_align_t) * 1024;
    struct alignas(overAlignedRequirement) BigBoy {
        int data;
    };
    struct BigBoys {
        BigBoy boys[2];
    };
    static_assert(alignof(BigBoys) == alignof(BigBoy));
    static_assert(alignof(BigBoys[2]) == alignof(BigBoy));

    X::declareDecoration<char>();
    auto bigBoy = X::declareDecoration<BigBoy>();
    X::declareDecoration<char>();
    auto bigBoys = X::declareDecoration<BigBoys>();

    X x;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&x[bigBoy]) % overAlignedRequirement, 0);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&x[bigBoys].boys[0]) % overAlignedRequirement, 0);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&x[bigBoys].boys[1]) % overAlignedRequirement, 0);
}

// Verify that a decorable with no decorations with alignment > 1 still has its buffer allocated
// with alignment >= alignof(void*)
TEST_F(DecorableTest, DefaultAlignment) {
    struct X : Decorable<X> {};
    X x;
    auto reg = decorable_detail::getRegistry<X>();
    ASSERT_EQ(reg.bufferAlignment() % alignof(void*), 0);
}

TEST_F(DecorableTest, MaplikeAccess) {
    struct X : Decorable<X> {};
    static auto d = X::declareDecoration<int>();

    X x;
    ASSERT_EQ(x[d], 0);
    x[d] = 123;
    ASSERT_EQ(x[d], 123);
}

TEST_F(DecorableTest, DecorationWithOwner) {
    struct X : public Decorable<X> {};
    struct Deco {};
    static auto d = X::declareDecoration<Deco>();
    static auto typeEq = []<typename A>(A&& a, A&& b) {
        return a == b;
    };

    X x;
    ASSERT_TRUE(typeEq(&d.owner(x[d]), &x)) << "ref {} {}"_format((void*)&d.owner(x[d]), (void*)&x);
    ASSERT_TRUE(typeEq(d.owner(&x[d]), &x)) << "ptr";
    ASSERT_TRUE(typeEq(&d.owner(std::as_const(x[d])), &std::as_const(x))) << "cref";
    ASSERT_TRUE(typeEq(d.owner(&std::as_const(x[d])), &std::as_const(x))) << "cptr";
}

TEST_F(DecorableTest, NonCopyableDecorable) {
    struct X : Decorable<X> {
        X() = default;
        X(const X&) = delete;
        X& operator=(const X&) = delete;
    };
    struct NonCopyable {
        NonCopyable() = default;
        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
        int value;
    };
    static auto d = X::declareDecoration<NonCopyable>();

    X x;
    ASSERT_EQ(x[d].value, 0);
    x[d].value = 123;
    ASSERT_EQ(x[d].value, 123);
}

TEST_F(DecorableTest, CopyableDecorable) {
    struct X : Decorable<X> {};
    static auto d1 = X::declareDecoration<A>();
    static auto d2 = X::declareDecoration<int>();

    X x1;
    x1[d1].value = 123;
    x1[d2] = 456;

    X x2(x1);
    ASSERT_EQ(stats.copyConstructed, 1);
    ASSERT_EQ(stats.copyAssigned, 0);
    ASSERT_EQ(x1[d1].value, x2[d1].value);
    ASSERT_EQ(x1[d2], x2[d2]);

    X x3;
    ASSERT_NE(x1[d1].value, x3[d1].value);
    ASSERT_NE(x1[d2], x3[d2]);

    x3 = x1;
    ASSERT_EQ(stats.copyConstructed, 1);
    ASSERT_EQ(stats.copyAssigned, 1);
    ASSERT_EQ(x1[d1].value, x3[d1].value);
    ASSERT_EQ(x1[d2], x3[d2]);
}

#if 0
TEST_F(DecorableTest, Inline) {
    class Inline : public Decorable<Inline> {
    public:
        explicit Inline(const AllocationInfo& spec) : Decorable<Inline>{spec} {
            std::cerr << "Inline: this={}\n"_format((void*)this);
        }
    };
    static auto d0 = Inline::declareDecoration<int>();
    static auto d1 = Inline::declareDecoration<std::array<char, 1024>>();
    static auto d2 = Inline::declareDecoration<int>();

    auto ptrDiff = [](const void* a, const void* b) {
        return (const char*)a - (const char*)b;
    };

    auto x = Inline::makeInline();
    std::cerr << "x = @{}\n"_format((void*)&*x);
    std::cerr << "x[d0]= @{} (x+{}), val={}\n"_format(
        (void*)&(*x)[d0], ptrDiff(&(*x)[d0], &*x), (*x)[d0]);
    std::cerr << "x[d1]= @{} (x+{})\n"_format((void*)&(*x)[d1], ptrDiff(&(*x)[d1], &*x));
    std::cerr << "x[d2]= @{} (x+{}), val={}\n"_format(
        (void*)&(*x)[d2], ptrDiff(&(*x)[d2], &*x), (*x)[d2]);
}
#endif

#if 0   // compile fail test. Enable manually to troubleshoot.
TEST_F(DecorableTest, Overaligned) {
    struct X : Decorable<X> {
        int x;
    };
    struct Overaligned {
        alignas(64) int value;
    };
    static auto d = X::declareDecoration<Overaligned>();
}
#endif  // 0

/**
 * Tests that a type that `decorable_detail::pretendTrivialInit` resolves to false for is properly
 * constructed.
 */
TEST_F(DecorableTest, DoNotZeroInitOther) {
    struct X : Decorable<X> {};
    class B : public A {
    public:
        B() : A() {
            value = 123;
        }
    };
    static auto decorator = X::declareDecoration<B>();
    ASSERT_EQ(stats.constructed, 0);
    X x;
    ASSERT_EQ(stats.constructed, 1);
    ASSERT_EQ(x[decorator].value, 123);
}

class DecorableZeroInitTest : public DecorableTest {
public:
    // Test decorated type needs copy assign and constructor removed so that types without copy
    // assign or constructor can be used as decorations (such as std::unique_ptr).
    struct X : Decorable<X> {
        X(const X&) = delete;
        X& operator=(const X&) = delete;
        X() = default;
    };

    template <typename DecorationType>
    bool isZeroBytes(const DecorationType& decoObj) {
        auto first = reinterpret_cast<const unsigned char*>(&decoObj);
        auto last = first + sizeof(decoObj);
        return std::find_if(first, last, [](auto c) { return c != 0; }) == last;
    }

    /**
     * Tests that value initializing the template type does not change the already zeroed buffer.
     * This ensures that construction of these types can safely be skipped.
     */
    template <typename T>
    void doBufferNoChangeTest() {
        auto buf = std::make_unique<std::array<unsigned char, sizeof(T)>>();
        invariant(isZeroBytes(*buf));
        invariant(reinterpret_cast<std::uintptr_t>(buf.get()) % alignof(T) == 0);
        auto ptr = new (buf->data()) T{};
        ScopeGuard ptrCleanup = [&] {
            ptr->~T();
        };
        ASSERT(isZeroBytes(*buf));
    }

    /**
     * Helper to test that constructors of the types used in `decorable_detail::pretendTrivialInit`
     * do not change the zeroed out decoration buffer.
     */
    template <typename DecorationType>
    void doZeroInitTest() {
        static auto decorator = X::template declareDecoration<DecorationType>();
        auto x = std::make_unique<X>();

        auto&& decorationObj = (*x)[decorator];
        ASSERT(isZeroBytes(decorationObj));
    }
};

TEST_F(DecorableZeroInitTest, BufferNoChangeBoostOptional) {
    doBufferNoChangeTest<boost::optional<A>>();
}

TEST_F(DecorableZeroInitTest, BufferNoChangeStdUniquePtr) {
    doBufferNoChangeTest<std::unique_ptr<A>>();
}

TEST_F(DecorableZeroInitTest, BufferNoChangeStdSharedPtr) {
    doBufferNoChangeTest<std::shared_ptr<A>>();
}

TEST_F(DecorableZeroInitTest, ZeroInitBoostOptionalDecoration) {
    doZeroInitTest<boost::optional<A>>();
}

TEST_F(DecorableZeroInitTest, ZeroInitStdUniquePtrDecoration) {
    doZeroInitTest<std::unique_ptr<A>>();
}

TEST_F(DecorableZeroInitTest, ZeroInitStdSharedPtrDecoration) {
    doZeroInitTest<std::shared_ptr<A>>();
}

}  // namespace
}  // namespace mongo
