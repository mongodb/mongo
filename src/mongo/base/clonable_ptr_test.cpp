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

#include "mongo/base/clonable_ptr.h"

#include <functional>
#include <memory>
#include <tuple>

#include "mongo/unittest/unittest.h"


namespace {

template <typename Test>
void runSyntaxTest(Test&& t) {
    if (false)
        t();
}

// These testing helper classes model various kinds of class which should be compatible with
// `mongo::clonable_ptr`.  The basic use cases satisfied by each class are described in each class's
// documentation.

// This class models the `Clonable` concept, and is used to test the simple case of `clonable_ptr`.
class ClonableTest {
private:
    std::string data =
        "This is the string data which is stored to make Functor Clonable need a complicated copy "
        "ctor.";

public:
    std::unique_ptr<ClonableTest> clone() const {
        return std::make_unique<ClonableTest>();
    }
};

// This class provides a member structure which models `CloneFactory<AltClonableTest>`.  The member
// structure is available under the expected member name of `clone_factory_type`.  The
// `CloneFactory` is stateless.
class AltClonableTest {
private:
    std::string data =
        "This is the string data which is stored to make Functor Clonable need a complicated copy "
        "ctor.";

public:
    struct clone_factory_type {
        std::unique_ptr<AltClonableTest> operator()(const AltClonableTest&) const {
            return std::make_unique<AltClonableTest>();
        }
    };
};

// This class requires a companion cloning function models `CloneFactory<Alt2ClonableTest>`.  There
// is an attendant specialization of the `mongo::clonable_traits` metafunction to provide the clone
// factory for this type.  That `CloneFactory` is stateless.
class Alt2ClonableTest {
private:
    std::string data =
        "This is the string data which is stored to make Functor Clonable need a complicated copy "
        "ctor.";
};
}  // namespace

namespace mongo {
// This specialization of the `mongo::clonable_traits` metafunction provides a model of a stateless
// `CloneFactory<Alt2ClonableTest>`
template <>
struct clonable_traits<::Alt2ClonableTest> {
    struct clone_factory_type {
        std::unique_ptr<Alt2ClonableTest> operator()(const Alt2ClonableTest&) const {
            return std::make_unique<Alt2ClonableTest>();
        }
    };
};
}  // namespace mongo

namespace {
// This class uses a stateful cloning function provided by the `getCloningFunction` static member.
// This stateful `CloneFactory<FunctorClonable>` must be passed to constructors of the
// `cloning_ptr`.
class FunctorClonable {
private:
    std::string data =
        "This is the string data which is stored to make Functor Clonable need a complicated copy "
        "ctor.";

public:
    using CloningFunctionType =
        std::function<std::unique_ptr<FunctorClonable>(const FunctorClonable&)>;

    static CloningFunctionType getCloningFunction() {
        return [](const FunctorClonable& c) { return std::make_unique<FunctorClonable>(c); };
    }
};


// This class uses a stateful cloning function provided by the `getCloningFunction` static member.
// This stateful `CloneFactory<FunctorWithDynamicStateClonable>` must be passed to constructors of
// the `cloning_ptr`.  The `CloneFactory` for this type dynamically updates its internal state.
// This is used to test cloning of objects that have dynamically changing clone factories.
class FunctorWithDynamicStateClonable {
private:
    std::string data =
        "This is the string data which is stored to make Functor Clonable need a complicated copy "
        "ctor.";

public:
    FunctorWithDynamicStateClonable(const FunctorWithDynamicStateClonable&) = delete;
    FunctorWithDynamicStateClonable() = default;

    FunctorWithDynamicStateClonable(const std::string& s) : data(s) {}

    using CloningFunctionType = std::function<std::unique_ptr<FunctorWithDynamicStateClonable>(
        const FunctorWithDynamicStateClonable&)>;

    static CloningFunctionType getCloningFunction() {
        return [calls = 0](const FunctorWithDynamicStateClonable& c) mutable {
            return std::make_unique<FunctorWithDynamicStateClonable>(c.data +
                                                                     std::to_string(++calls));
        };
    }
};

// This class models `Clonable`, with a return from clone which is
// `Constructible<std::unique_ptr<RawPointerClonable>>` but isn't
// `std::unique_ptr<RawPointerClonable>`.  This is used to test that the `clonable_ptr` class does
// not expect `RawPointerClonable::clone() const` to return a model of
// `UniquePtr<RawPointerClonable>`
class RawPointerClonable {
public:
    RawPointerClonable* clone() const {
        return new RawPointerClonable;
    }
};

// This class models `Clonable`, with a return from clone which is
// `Constructible<std::unique_ptr<UniquePtrClonable>>` because it is
// `std::unique_ptr<UniquePtrClonable>`.  This is used to test that the `clonable_ptr` class can
// use a `UniquePtrClonable::clone() const` that returns a model of
// `UniquePtr<UniquePtrClonable>`
class UniquePtrClonable {
public:
    std::unique_ptr<UniquePtrClonable> clone() const {
        return std::make_unique<UniquePtrClonable>();
    }
};

TEST(ClonablePtrTest, syntax_smoke_test) {
// TODO: Either add a compressed pair type for optimization, or wait for MSVC to get this feature by
//       default.  MSVC doesn't make its tuple compressed, which causes this test to fail on MSVC.
#ifndef _MSC_VER
    {
        mongo::clonable_ptr<ClonableTest> p;

        p = std::make_unique<ClonableTest>();

        mongo::clonable_ptr<ClonableTest> p2 = p;

        ASSERT_TRUE(p != p2);

        static_assert(sizeof(p) == sizeof(ClonableTest*),
                      "`mongo::clonable_ptr< T >` should be `sizeof` a pointer when there is no "
                      "CloneFactory");
    }
#endif

    {
        mongo::clonable_ptr<AltClonableTest> p;

        p = std::make_unique<AltClonableTest>();

        mongo::clonable_ptr<AltClonableTest> p2 = p;

        ASSERT_TRUE(p != p2);
    }

    {
        mongo::clonable_ptr<Alt2ClonableTest> p;

        p = std::make_unique<Alt2ClonableTest>();

        mongo::clonable_ptr<Alt2ClonableTest> p2 = p;

        ASSERT_TRUE(p != p2);
    }

    {
        mongo::clonable_ptr<FunctorClonable, FunctorClonable::CloningFunctionType> p{
            FunctorClonable::getCloningFunction()};

        p = std::make_unique<FunctorClonable>();

        mongo::clonable_ptr<FunctorClonable, FunctorClonable::CloningFunctionType> p2 = p;

        ASSERT_TRUE(p != p2);

        mongo::clonable_ptr<FunctorClonable, FunctorClonable::CloningFunctionType> p3{
            FunctorClonable::getCloningFunction()};

        auto tmp = std::make_unique<FunctorClonable>();
        p3 = std::move(tmp);

        ASSERT_TRUE(p != p2);
        ASSERT_TRUE(p2 != p3);
        ASSERT_TRUE(p != p3);
    }
}

// These tests check that all expected valid syntactic forms of use for the
// `mongo::clonable_ptr<Clonable>` are valid.  These tests assert nothing but provide a single
// unified place to check the syntax of this component.  Build failures in these parts indicate that
// a change to the component has broken an expected valid syntactic usage.  Any expected valid usage
// which is not in this list should be added.
namespace SyntaxTests {
template <typename Clonable>
void construction() {
    // Test default construction
    { mongo::clonable_ptr<Clonable>{}; }

    // Test construction from a nullptr
    { mongo::clonable_ptr<Clonable>{nullptr}; }

    // Test construction from a Clonable pointer.
    {
        Clonable* const local = nullptr;
        mongo::clonable_ptr<Clonable>{local};
    }

    // Test move construction.
    { std::ignore = mongo::clonable_ptr<Clonable>{mongo::clonable_ptr<Clonable>{}}; }

    // Test copy construction.
    {
        mongo::clonable_ptr<Clonable> a;
        mongo::clonable_ptr<Clonable> b{a};
    }

    // Test move assignment.
    {
        mongo::clonable_ptr<Clonable> a;
        a = mongo::clonable_ptr<Clonable>{};
    }

    // Test copy assignment.
    {
        mongo::clonable_ptr<Clonable> a;
        mongo::clonable_ptr<Clonable> b;
        b = a;
    }

    // Test unique pointer construction
    { mongo::clonable_ptr<Clonable>{std::make_unique<Clonable>()}; }

    // Test unique pointer construction (conversion)
    {
        auto acceptor = [](const mongo::clonable_ptr<Clonable>&) {};
        acceptor(std::make_unique<Clonable>());
    }

    // Test non-conversion pointer construction
    { static_assert(!std::is_convertible<Clonable*, mongo::clonable_ptr<Clonable>>::value); }

    // Test conversion unique pointer construction
    {
        static_assert(
            std::is_convertible<std::unique_ptr<Clonable>, mongo::clonable_ptr<Clonable>>::value);
    }
}

// Tests that syntactic forms that require augmented construction are proper
template <typename Clonable, typename CloneFactory>
void augmentedConstruction() {
    // Test default construction
    {
        static_assert(
            !std::is_default_constructible<mongo::clonable_ptr<Clonable, CloneFactory>>::value);
    }

    // Test Clone Factory construction
    { mongo::clonable_ptr<Clonable, CloneFactory>{Clonable::getCloningFunction()}; }

// TODO: Revist this when MSVC's enable-if and deletion on ctors works.
#ifndef _MSC_VER
    // Test non-construction from a nullptr
    {
        static_assert(!std::is_constructible<mongo::clonable_ptr<Clonable, CloneFactory>,
                                             std::nullptr_t>::value);
    }
#endif

    // Test construction from a nullptr with factory
    { mongo::clonable_ptr<Clonable, CloneFactory>{nullptr, Clonable::getCloningFunction()}; }

// TODO: Revist this when MSVC's enable-if and deletion on ctors works.
#ifndef _MSC_VER
    // Test construction from a raw Clonable pointer.
    {
        static_assert(
            !std::is_constructible<mongo::clonable_ptr<Clonable, CloneFactory>, Clonable*>::value);
    }
#endif


    // Test initialization of a raw Clonable pointer with factory, using reset.
    {
        Clonable* const local = nullptr;
        mongo::clonable_ptr<Clonable, CloneFactory> p{Clonable::getCloningFunction()};
        p.reset(local);
    }

    // Test move construction.
    {
        std::ignore = mongo::clonable_ptr<Clonable, CloneFactory>{
            mongo::clonable_ptr<Clonable, CloneFactory>{Clonable::getCloningFunction()}};
    }

    // Test copy construction.
    {
        mongo::clonable_ptr<Clonable, CloneFactory> a{Clonable::getCloningFunction()};
        mongo::clonable_ptr<Clonable, CloneFactory> b{a};
    }

    // Test augmented copy construction.
    {
        mongo::clonable_ptr<Clonable, CloneFactory> a{Clonable::getCloningFunction()};
        mongo::clonable_ptr<Clonable, CloneFactory> b{a, Clonable::getCloningFunction()};
    }


    // Test move assignment.
    {
        mongo::clonable_ptr<Clonable, CloneFactory> a{Clonable::getCloningFunction()};
        a = mongo::clonable_ptr<Clonable, CloneFactory>{Clonable::getCloningFunction()};
    }

    // Test copy assignment.
    {
        mongo::clonable_ptr<Clonable, CloneFactory> a{Clonable::getCloningFunction()};
        mongo::clonable_ptr<Clonable, CloneFactory> b{Clonable::getCloningFunction()};
        b = a;
    }

    // Test unique pointer construction
    {
        mongo::clonable_ptr<Clonable, CloneFactory>{std::make_unique<Clonable>(),
                                                    Clonable::getCloningFunction()};
    }

    // Test augmented unique pointer construction
    {
        mongo::clonable_ptr<Clonable, CloneFactory>{std::make_unique<Clonable>(),
                                                    Clonable::getCloningFunction()};
    }

    // Test non-conversion pointer construction
    {
        static_assert(
            !std::is_convertible<mongo::clonable_ptr<Clonable, CloneFactory>, Clonable*>::value);
    }

    // Test non-conversion from factory
    {
        static_assert(
            !std::is_convertible<mongo::clonable_ptr<Clonable, CloneFactory>, CloneFactory>::value);
    }

    // Test conversion unique pointer construction
    {
        static_assert(!std::is_convertible<std::unique_ptr<Clonable>,
                                           mongo::clonable_ptr<Clonable, CloneFactory>>::value);
    }
}

template <typename Clonable>
void pointerOperations() {
    mongo::clonable_ptr<Clonable> a;

    // Test `.get()` functionality:
    {
        Clonable* p = a.get();
        (void)p;
    }

    // Test `->` functionality
    {
        // We don't actually want to call the dtor, but we want the compiler to check that we
        // have.
        if (false) {
            a->~Clonable();
        }
    }

    // Test `*` functionality
    Clonable& r = *a;
    (void)r;

    // Test reset functionality
    {
        a.reset();
        a.reset(nullptr);
        a.reset(new Clonable);
    }
}

template <typename Clonable>
void equalityOperations() {
    mongo::clonable_ptr<Clonable> a;
    mongo::clonable_ptr<Clonable> b;

    std::unique_ptr<Clonable> ua;

    // Test equality expressions
    {
        (void)(a == a);
        (void)(a == b);
        (void)(b == a);

        (void)(a == ua);
        (void)(ua == a);

        (void)(nullptr == a);
        (void)(a == nullptr);
    }

    // Test inequality expressions
    {
        (void)(a != a);
        (void)(a != b);
        (void)(b != a);

        (void)(a != ua);
        (void)(ua != a);

        (void)(nullptr == a);
        (void)(a == nullptr);
    }
}
}  // namespace SyntaxTests

TEST(ClonablePtrSyntaxTests, construction) {
    runSyntaxTest(SyntaxTests::construction<ClonableTest>);
    runSyntaxTest(SyntaxTests::construction<AltClonableTest>);
    runSyntaxTest(SyntaxTests::construction<Alt2ClonableTest>);
    runSyntaxTest(SyntaxTests::construction<RawPointerClonable>);
    runSyntaxTest(SyntaxTests::construction<UniquePtrClonable>);
}

TEST(ClonablePtrSyntaxTests, augmentedConstruction) {
    runSyntaxTest(
        SyntaxTests::augmentedConstruction<FunctorClonable, FunctorClonable::CloningFunctionType>);
    runSyntaxTest(
        SyntaxTests::augmentedConstruction<FunctorWithDynamicStateClonable,
                                           FunctorWithDynamicStateClonable::CloningFunctionType>);
}

TEST(ClonablePtrSyntaxTests, pointerOperations) {
    runSyntaxTest(SyntaxTests::pointerOperations<ClonableTest>);
    runSyntaxTest(SyntaxTests::pointerOperations<AltClonableTest>);
    runSyntaxTest(SyntaxTests::pointerOperations<Alt2ClonableTest>);
    runSyntaxTest(SyntaxTests::pointerOperations<RawPointerClonable>);
    runSyntaxTest(SyntaxTests::pointerOperations<UniquePtrClonable>);
}

TEST(ClonablePtrSyntaxTests, equalityOperations) {
    runSyntaxTest(SyntaxTests::equalityOperations<ClonableTest>);
    runSyntaxTest(SyntaxTests::equalityOperations<AltClonableTest>);
    runSyntaxTest(SyntaxTests::equalityOperations<Alt2ClonableTest>);
    runSyntaxTest(SyntaxTests::equalityOperations<RawPointerClonable>);
    runSyntaxTest(SyntaxTests::equalityOperations<UniquePtrClonable>);
}

namespace BehaviorTests {
class DetectDestruction {
public:
    static int activeCount;

    static void resetCount() {
        activeCount = 0;
    }

    ~DetectDestruction() {
        --activeCount;
    }

    DetectDestruction(const DetectDestruction&) {
        ++activeCount;
    }

    DetectDestruction& operator=(const DetectDestruction&) = delete;

    DetectDestruction(DetectDestruction&&) = delete;
    DetectDestruction& operator=(DetectDestruction&&) = delete;

    DetectDestruction() {
        ++activeCount;
    }

    std::unique_ptr<DetectDestruction> clone() const {
        return std::make_unique<DetectDestruction>(*this);
    }
};

int DetectDestruction::activeCount = 0;

struct DestructionGuard {
    DestructionGuard(const DestructionGuard&) = delete;
    DestructionGuard& operator=(const DestructionGuard&) = delete;

    DestructionGuard() {
        DetectDestruction::resetCount();
    }

    ~DestructionGuard() {
        ASSERT_EQ(DetectDestruction::activeCount, 0);
    }
};

TEST(ClonablePtrTest, basic_construction_test) {
    // Do not default construct the object
    {
        DestructionGuard check;
        mongo::clonable_ptr<DetectDestruction> p;
        ASSERT_EQ(DetectDestruction::activeCount, 0);
    }

    // Do not make unnecessary copies of the object from ptr
    {
        DestructionGuard check;
        mongo::clonable_ptr<DetectDestruction> p{new DetectDestruction};
        ASSERT_EQ(DetectDestruction::activeCount, 1);
    }

    // Do not make unnecessary copies of the object from unique_ptr
    {
        DestructionGuard check;
        mongo::clonable_ptr<DetectDestruction> p{std::make_unique<DetectDestruction>()};
        ASSERT_EQ(DetectDestruction::activeCount, 1);
    }
    {
        DestructionGuard check;
        mongo::clonable_ptr<DetectDestruction> p = std::make_unique<DetectDestruction>();
        ASSERT_EQ(DetectDestruction::activeCount, 1);
    }

    // Two separate constructions are unlinked
    {
        DestructionGuard check;

        mongo::clonable_ptr<DetectDestruction> p1{std::make_unique<DetectDestruction>()};
        ASSERT_EQ(DetectDestruction::activeCount, 1);

        {
            mongo::clonable_ptr<DetectDestruction> p2{std::make_unique<DetectDestruction>()};
            ASSERT_EQ(DetectDestruction::activeCount, 2);
        }
        ASSERT_EQ(DetectDestruction::activeCount, 1);
    }

    // Two separate constructions can have opposite order and be unlinked
    {
        DestructionGuard check;

        auto p1 = std::make_unique<mongo::clonable_ptr<DetectDestruction>>(
            std::make_unique<DetectDestruction>());
        ASSERT_EQ(DetectDestruction::activeCount, 1);

        auto p2 = std::make_unique<mongo::clonable_ptr<DetectDestruction>>(
            std::make_unique<DetectDestruction>());
        ASSERT_EQ(DetectDestruction::activeCount, 2);

        p1.reset();
        ASSERT_EQ(DetectDestruction::activeCount, 1);

        p2.reset();
        ASSERT_EQ(DetectDestruction::activeCount, 0);
    }
}

// TODO: Bring in an "equivalence class for equality predicate testing" framework.
// Equals and Not Equals need to be tested independently -- It is not valid to assume that equals
// and not equals are correctly implemented as complimentary predicates.  Equality must be
// reflexive, symmetric and transitive.  This requres several instances that all have the same
// value.  Simply testing "2 == 2" and "3 != 2" is insufficient.  Every combination of position and
// equality must be tested to come out as expected.
//
// Consider that with equality it is important to make sure that `a == b` has the same meaning as `b
// == a`.  It is also necessary to check that `a == b` and `b == c` and `a == c` is true, when all
// three are equal, and to do so in reverse: `b == a` and `c == b` and `c == a`.  Further, the
// relationships above have to hold for multiple cases.  Similar cases need to be tested for
// inequality.
//
// Further, equality is an incredibly important operation to test completely and thoroughly --
// besides being a critical element in code using any value modeling type, it also is the keystone
// in any testing schedule for a copyable and movable value type.  Almost all testing of behavior
// relies upon being able to detect fundamental differences in value.  In order to provide this
// correctly, we provide a full battery of tests for equality in all mathematically relevant
// situations.  For this equality testing schedule to be correct, we require a mechanism to
// initialize objects (and references to those objects) which have predictable value.  These
// predictable values are then used to test known equality expressions for the correct evaluation.
//
// All other tests can then just use equality to verify that an object is in the desired state.
// This greatly simplifies testing and also makes tests more precise.
TEST(ClonablePtrTest, basicEqualityTest) {
    DestructionGuard check;

    mongo::clonable_ptr<DetectDestruction> n1;
    mongo::clonable_ptr<DetectDestruction> n2;
    mongo::clonable_ptr<DetectDestruction> n3;

    mongo::clonable_ptr<DetectDestruction> a = std::make_unique<DetectDestruction>();
    mongo::clonable_ptr<DetectDestruction> b = std::make_unique<DetectDestruction>();
    mongo::clonable_ptr<DetectDestruction> c = std::make_unique<DetectDestruction>();

    const mongo::clonable_ptr<DetectDestruction>& ap = a;
    const mongo::clonable_ptr<DetectDestruction>& bp = b;
    const mongo::clonable_ptr<DetectDestruction>& cp = c;
    const mongo::clonable_ptr<DetectDestruction>& ap2 = a;
    const mongo::clonable_ptr<DetectDestruction>& bp2 = b;
    const mongo::clonable_ptr<DetectDestruction>& cp2 = c;

    // Equals operator

    // Identity checks

    ASSERT(n1 == n1);
    ASSERT(n2 == n2);
    ASSERT(n3 == n3);

    ASSERT(a == a);
    ASSERT(b == b);
    ASSERT(c == c);

    // Same value checks.  (Because unique pointers should never be the same value, we have to use
    // references.)

    ASSERT(n1 == n2);
    ASSERT(n1 == n3);

    ASSERT(n2 == n1);
    ASSERT(n2 == n3);

    ASSERT(n3 == n1);
    ASSERT(n3 == n2);

    ASSERT(a == ap);
    ASSERT(a == ap2);
    ASSERT(ap == a);
    ASSERT(ap == ap2);
    ASSERT(ap2 == a);
    ASSERT(ap2 == ap);

    ASSERT(b == bp);
    ASSERT(b == bp2);
    ASSERT(bp == b);
    ASSERT(bp == bp2);
    ASSERT(bp2 == b);
    ASSERT(bp2 == bp);

    ASSERT(c == cp);
    ASSERT(c == cp2);
    ASSERT(cp == c);
    ASSERT(cp == cp2);
    ASSERT(cp2 == c);
    ASSERT(cp2 == cp);

    // Different value checks:

    ASSERT(!(a == n1));
    ASSERT(!(b == n1));
    ASSERT(!(c == n1));
    ASSERT(!(a == n2));
    ASSERT(!(b == n2));
    ASSERT(!(c == n2));

    ASSERT(!(n1 == a));
    ASSERT(!(n1 == b));
    ASSERT(!(n1 == c));
    ASSERT(!(n2 == a));
    ASSERT(!(n2 == b));
    ASSERT(!(n2 == c));

    ASSERT(!(a == b));
    ASSERT(!(a == c));

    ASSERT(!(b == a));
    ASSERT(!(b == c));

    ASSERT(!(c == a));
    ASSERT(!(c == b));

    // Not Equals operator

    // Identity checks

    ASSERT(!(n1 != n1));
    ASSERT(!(n2 != n2));
    ASSERT(!(n3 != n3));

    ASSERT(!(a != a));
    ASSERT(!(b != b));
    ASSERT(!(c != c));

    // Same value checks.  (Because unique pointers should never be the same value, we have to use
    // references.)

    ASSERT(!(n1 != n2));
    ASSERT(!(n1 != n3));

    ASSERT(!(n2 != n1));
    ASSERT(!(n2 != n3));

    ASSERT(!(n3 != n1));
    ASSERT(!(n3 != n2));

    ASSERT(!(a != ap));
    ASSERT(!(a != ap2));
    ASSERT(!(ap != a));
    ASSERT(!(ap != ap2));
    ASSERT(!(ap2 != a));
    ASSERT(!(ap2 != ap));

    ASSERT(!(b != bp));
    ASSERT(!(b != bp2));
    ASSERT(!(bp != b));
    ASSERT(!(bp != bp2));
    ASSERT(!(bp2 != b));
    ASSERT(!(bp2 != bp));

    ASSERT(!(c != cp));
    ASSERT(!(c != cp2));
    ASSERT(!(cp != c));
    ASSERT(!(cp != cp2));
    ASSERT(!(cp2 != c));
    ASSERT(!(cp2 != cp));

    // Different value checks:

    ASSERT(a != n1);
    ASSERT(b != n1);
    ASSERT(c != n1);
    ASSERT(a != n2);
    ASSERT(b != n2);
    ASSERT(c != n2);

    ASSERT(n1 != a);
    ASSERT(n1 != b);
    ASSERT(n1 != c);
    ASSERT(n2 != a);
    ASSERT(n2 != b);
    ASSERT(n2 != c);

    ASSERT(a != b);
    ASSERT(a != c);

    ASSERT(b != a);
    ASSERT(b != c);

    ASSERT(c != a);
    ASSERT(c != b);
}

// TODO: all other forms of equality with other types (`std::nullptr_t` and `std::unique_ptr< T >`)
// need testing still.

TEST(ClonablePtrTest, ownershipStabilityTest) {
    {
        DestructionGuard check;

        auto ptr_init = std::make_unique<DetectDestruction>();
        const auto* rp = ptr_init.get();

        mongo::clonable_ptr<DetectDestruction> cp = std::move(ptr_init);

        ASSERT(rp == cp.get());

        mongo::clonable_ptr<DetectDestruction> cp2 = std::move(cp);

        ASSERT(rp == cp2.get());

        mongo::clonable_ptr<DetectDestruction> cp3;

        ASSERT(nullptr == cp3);

        cp3 = std::move(cp2);

        ASSERT(rp == cp3.get());
    }

    {
        auto ptr_init = std::make_unique<DetectDestruction>();
        const auto* rp = ptr_init.get();

        mongo::clonable_ptr<DetectDestruction> cp{ptr_init.release()};

        ASSERT(rp == cp.get());

        mongo::clonable_ptr<DetectDestruction> cp2 = std::move(cp);

        ASSERT(rp == cp2.get());

        mongo::clonable_ptr<DetectDestruction> cp3;

        ASSERT(nullptr == cp3.get());

        cp3 = std::move(cp2);

        ASSERT(rp == cp3.get());
    }
}

class ClonableObject {
private:
    int value = 0;

public:
    // ClonableObject( const ClonableObject & ) { abort(); }
    ClonableObject() = default;
    explicit ClonableObject(const int v) : value(v) {}

    std::unique_ptr<ClonableObject> clone() const {
        return std::make_unique<ClonableObject>(*this);
    }

    auto make_equality_lens() const -> decltype(std::tie(this->value)) {
        return std::tie(value);
    }
};

bool operator==(const ClonableObject& lhs, const ClonableObject& rhs) {
    return lhs.make_equality_lens() == rhs.make_equality_lens();
}

bool operator!=(const ClonableObject& lhs, const ClonableObject& rhs) {
    return !(lhs == rhs);
}

TEST(ClonablePtrTest, noObjectCopySemanticTest) {
    mongo::clonable_ptr<ClonableObject> p;

    mongo::clonable_ptr<ClonableObject> p2 = p;
    ASSERT(p == p2);

    mongo::clonable_ptr<ClonableObject> p3;

    p3 = p;
    ASSERT(p == p3);
}

TEST(ClonablePtrTest, objectCopySemanticTest) {
    mongo::clonable_ptr<ClonableObject> p = std::make_unique<ClonableObject>(1);
    mongo::clonable_ptr<ClonableObject> q = std::make_unique<ClonableObject>(2);
    ASSERT(p != q);
    ASSERT(*p != *q);

    mongo::clonable_ptr<ClonableObject> p2 = p;
    ASSERT(p != p2);
    ASSERT(*p == *p2);

    mongo::clonable_ptr<ClonableObject> q2 = q;
    ASSERT(q2 != q);
    ASSERT(q2 != p);
    ASSERT(q2 != p2);
    ASSERT(*q2 == *q);

    q2 = p2;
    ASSERT(q2 != q);
    ASSERT(q2 != p);
    ASSERT(q2 != p2);
    ASSERT(*q2 == *p2);
}

class Interface {
public:
    virtual ~Interface() = default;
    virtual void consumeText(const std::string& message) = 0;
    virtual std::string produceText() = 0;

    std::unique_ptr<Interface> clone() const {
        return std::unique_ptr<Interface>{this->clone_impl()};
    }

private:
    virtual Interface* clone_impl() const = 0;
};

class GeneratorImplementation : public Interface {
private:
    const std::string root;
    int generation = 0;

    GeneratorImplementation* clone_impl() const {
        return new GeneratorImplementation{*this};
    }

public:
    explicit GeneratorImplementation(const std::string& m) : root(m) {}

    void consumeText(const std::string&) override {}

    std::string produceText() override {
        return root + std::to_string(++generation);
    }
};

class StorageImplementation : public Interface {
private:
    std::string store;

    StorageImplementation* clone_impl() const {
        return new StorageImplementation{*this};
    }

public:
    void consumeText(const std::string& m) override {
        store = m;
    }
    std::string produceText() override {
        return store;
    }
};


TEST(ClonablePtrSimpleTest, simpleUsageExample) {
    mongo::clonable_ptr<Interface> source;
    mongo::clonable_ptr<Interface> sink;

    mongo::clonable_ptr<Interface> instance = std::make_unique<StorageImplementation>();

    sink = instance;

    ASSERT(instance.get() != sink.get());

    instance = std::make_unique<GeneratorImplementation>("base message");


    source = std::move(instance);


    sink->consumeText(source->produceText());
}

}  // namespace BehaviorTests
}  // namespace
