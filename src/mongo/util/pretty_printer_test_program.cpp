// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debugger.h"
#include "mongo/util/decorable.h"
#include "mongo/util/string_map.h"

#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#if defined(__clang__)
#define clang_optnone __attribute__((optnone))
#else
#define clang_optnone
#endif
#pragma GCC push_options
#pragma GCC optimize("O0")

struct MyDecorable : mongo::Decorable<MyDecorable> {};

class testClass {
public:
    static void print_member() {
        std::cout << testClass::static_member << std::endl;
    }

private:
    static const unsigned static_member;
};
const unsigned testClass::static_member(128);

struct NonEmptyHash {
    std::size_t operator()(const std::string& key) const {
        return 0;
    }

    int x = 0;
};

struct NonEmptyMapEq {
    // This using directive activates heterogeneous lookup in the hash table
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const {
        return lhs == rhs;
    }

    int x = 0;
};

template <typename T>
class NonEmptyAlloc {
public:
    using value_type = T;

    NonEmptyAlloc() = default;

    template <typename U>
    constexpr NonEmptyAlloc(const NonEmptyAlloc<U>&) noexcept {}

    T* allocate(std::size_t n) {
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        std::allocator<T>{}.deallocate(p, n);
    }

    template <typename U>
    bool operator==(const NonEmptyAlloc<U>&) const {
        return true;
    }

    int x = 0;
};

class IntWrapper {
public:
    IntWrapper(int i) : _i(i) {}

private:
    int _i;
};

auto intVec = MyDecorable::declareDecoration<std::vector<int>>();
auto str1 = MyDecorable::declareDecoration<std::string>();
auto str2 = MyDecorable::declareDecoration<std::string>();

constexpr auto testData = mongo::namespace_string_data::makeNsData<9, 4>("constexpr", "name");
constexpr mongo::NamespaceString kConstNs(testData.data(), testData.size());

template <typename T>
struct ContainerValue;

template <>
struct ContainerValue<int> {
    static int make(int i) {
        return i;
    }
};

template <>
struct ContainerValue<std::string> {
    // Produces "a", "b", ... "z", then "aa", "bb", and so on, so that arbitrarily many
    // distinct strings are available.
    static std::string make(int i) {
        invariant(i >= 0);
        return std::string(i / 26 + 1, static_cast<char>('a' + i % 26));
    }
};

template <typename K, typename V>
struct ContainerValue<std::pair<K, V>> {
    static std::pair<K, V> make(int i) {
        return {ContainerValue<std::remove_const_t<K>>::make(i),
                ContainerValue<std::remove_const_t<V>>::make(i)};
    }
};

// The part of an element that identifies it for erase(). For a set that is the element
// itself, and for a map it is the key.
template <typename T>
struct ContainerKey {
    using type = T;

    static type make(int i) {
        return ContainerValue<type>::make(i);
    }
};

template <typename K, typename V>
struct ContainerKey<std::pair<K, V>> {
    using type = std::remove_const_t<K>;

    static type make(int i) {
        return ContainerValue<type>::make(i);
    }
};

// A bunch of containers with content that pretty_printer_test.py expects. Each member is
// constructed and mutated in a different way to cover a range of possible states such as
// containers with few elements that may be in small-object optimization mode and containers
// that contain tombstones for deleted elements.
template <typename Container>
class AbslContainerStates {
public:
    using Element = Container::value_type;

    static Element elem(int i) {
        return ContainerValue<Element>::make(i);
    }

    static void insertRange(Container& c, int first, int last) {
        for (int i = first; i < last; i++) {
            c.insert(elem(i));
        }
    }

    // Erases elements [first, last), all of which must be present.
    static void eraseRange(Container& c, int first, int last) {
        for (int i = first; i < last; i++) {
            invariant(c.erase(ContainerKey<Element>::make(i)) == 1);
        }
    }

    AbslContainerStates() {
        insertRange(movedFrom, 0, 3);
        auto movedTo = std::move(movedFrom);

        insert1.insert(elem(0));

        insert1ThenDelete.insert(elem(0));
        insert1ThenDelete.erase(insert1ThenDelete.begin());

        insert1ThenClear.insert(elem(0));
        insert1ThenClear.clear();

        insert1ThenDeleteThenInsert.insert(elem(0));
        insert1ThenDeleteThenInsert.erase(insert1ThenDeleteThenInsert.begin());
        insert1ThenDeleteThenInsert.insert(elem(1));

        insert1ThenClearThenInsert.insert(elem(0));
        insert1ThenClearThenInsert.clear();
        insert1ThenClearThenInsert.insert(elem(1));

        // A container with more than one element is never in SOO mode, so these cover the
        // smallest heap-allocated (i.e., non-SOO) tables for SOO-eligible container types.
        insertRange(insert2, 0, 2);

        insertRange(insert3, 0, 3);

        insertRange(insert3Delete1, 0, 3);
        eraseRange(insert3Delete1, 0, 1);

        // Size one but not in SOO mode, as the capacity is still three.
        insertRange(insert3Delete2, 0, 3);
        eraseRange(insert3Delete2, 0, 2);

        insertRange(insert3DeleteAll, 0, 3);
        eraseRange(insert3DeleteAll, 0, 3);

        insertRange(insert3DeleteAllReinsert, 0, 3);
        eraseRange(insert3DeleteAllReinsert, 0, 3);
        insert3DeleteAllReinsert.insert(elem(3));

        insertRange(insert8, 0, 8);

        insertRange(insert8Delete4, 0, 8);
        eraseRange(insert8Delete4, 0, 4);

        insertRange(insert8Delete4Reinsert, 0, 8);
        eraseRange(insert8Delete4Reinsert, 0, 4);
        insertRange(insert8Delete4Reinsert, 8, 12);

        insertRange(insert8DeleteAll, 0, 8);
        eraseRange(insert8DeleteAll, 0, 8);

        insertRange(insert8DeleteAllReinsert, 0, 8);
        eraseRange(insert8DeleteAllReinsert, 0, 8);
        insert8DeleteAllReinsert.insert(elem(8));

        // A table holding 28 elements has a capacity of 31, while a table with 29 elements
        // goes to the next capacity up (63). The condition for using tombstones is that
        // the capacity exceeds the size of a "probing group", and 31 should be big enough
        // for that (the size of a group is platform-specific depending on available SIMD
        // primitives).
        insertRange(insert28, 0, 28);

        // This table should have tombstones.
        insertRange(insert28Delete4, 0, 28);
        eraseRange(insert28Delete4, 0, 4);

        // This table should exercise tombstones being reclaimed.
        insertRange(insert28Delete4Reinsert, 0, 28);
        eraseRange(insert28Delete4Reinsert, 0, 4);
        insertRange(insert28Delete4Reinsert, 28, 32);
    }

    Container empty;
    Container movedFrom;
    Container insert1;
    Container insert1ThenDelete;
    Container insert1ThenClear;
    Container insert1ThenDeleteThenInsert;
    Container insert1ThenClearThenInsert;
    Container insert2;
    Container insert3;
    Container insert3Delete1;
    Container insert3Delete2;
    Container insert3DeleteAll;
    Container insert3DeleteAllReinsert;
    Container insert8;
    Container insert8Delete4;
    Container insert8Delete4Reinsert;
    Container insert8DeleteAll;
    Container insert8DeleteAllReinsert;
    Container insert28;
    Container insert28Delete4;
    Container insert28Delete4Reinsert;
};

MyDecorable d1;
int clang_optnone main(int argc, char** argv) {
    std::set<int> set_type = {1, 2, 3, 4};
    std::unique_ptr<int> up(new int);
    intVec(d1) = {123, 213, 312};
    str1(d1) = "hello";
    str2(d1) = "world";
    mongo::TenantId tenantId{mongo::OID{"6491a2112ef5c818703bf2a7"}};
    mongo::DatabaseName dbName =
        mongo::DatabaseName::createDatabaseName_forTest(boost::none, "foo");
    mongo::DatabaseName dbNameWithTenantId =
        mongo::DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    mongo::NamespaceString nss =
        mongo::NamespaceString::createNamespaceString_forTest(boost::none, "foo.ba");
    mongo::NamespaceString nssWithTenantId =
        mongo::NamespaceString::createNamespaceString_forTest(tenantId, "foo.barbaz");
    mongo::NamespaceString longNss = mongo::NamespaceString::createNamespaceString_forTest(
        boost::none, "longdatabasenamewithoutsmallstring.longcollection");
    mongo::NamespaceString constCopy = kConstNs;

    // A container uses the small object optimization (SOO) when its slot type fits in the space
    // otherwise occupied by two pointers, that is when sizeof(slot_type) <= 16 and
    // alignof(slot_type) <= 8. The flat containers store their elements directly in the
    // slots while the node containers store a pointer in each slot. As a result,
    // flat containers are only SOO-eligible when their keys and values are small enough,
    // node containers are always SOO-eligible as a single pointer is small enough.
    AbslContainerStates<mongo::StringMap<int>> stringIntMaps;
    AbslContainerStates<mongo::StringMap<std::string>> stringStringMaps;
    AbslContainerStates<absl::flat_hash_map<int, std::string>> intStringMaps;
    AbslContainerStates<mongo::StringSet> stringSets;

    // These two should be small enough to be SOO-eligible.
    AbslContainerStates<absl::flat_hash_map<int, int>> intIntMaps;
    AbslContainerStates<absl::flat_hash_set<int>> intSets;

    // The node containers hold a pointer in each slot, so they are always SOO-eligible.
    AbslContainerStates<absl::node_hash_map<std::string, int>> stringIntNodeMaps;
    AbslContainerStates<absl::node_hash_map<std::string, std::string>> stringStringNodeMaps;
    AbslContainerStates<absl::node_hash_map<int, int>> intIntNodeMaps;
    AbslContainerStates<absl::node_hash_map<int, std::string>> intStringNodeMaps;
    AbslContainerStates<absl::node_hash_set<std::string>> stringNodeSets;
    AbslContainerStates<absl::node_hash_set<int>> intNodeSets;

    // A custom hasher shouldn't break printers. This one makes everything collide, but
    // that doesn't break printing.
    AbslContainerStates<absl::flat_hash_set<std::string, NonEmptyHash, mongo::StringMapEq>>
        nonEmptyHashSets;
    AbslContainerStates<absl::flat_hash_set<std::string, mongo::StringMapHasher, NonEmptyMapEq>>
        nonEmptyEqSets;
    AbslContainerStates<absl::flat_hash_set<std::string,
                                            mongo::StringMapHasher,
                                            mongo::StringMapEq,
                                            NonEmptyAlloc<std::string>>>
        nonEmptyAllocSets;

    boost::optional<int> optTypeNone;
    boost::optional<int> optTypeValue{1};

    boost::optional<IntWrapper> wrappedOptTypeNone;
    boost::optional<IntWrapper> wrappedOptTypeValue{IntWrapper{1}};

    auto obj = BSON("x" << "1" << "sub" << BSON("y" << "1"));

    auto unownedObj = mongo::BSONObj(obj.objdata());

    mongo::breakpoint();

    return 0;
}

#pragma GCC pop_options
