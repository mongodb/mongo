/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/debugger.h"
#include "mongo/util/decorable.h"
#include "mongo/util/string_map.h"

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

    bool operator()(std::string lhs, std::string rhs) const {
        return true;
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
        return nullptr;
    }

    void deallocate(T* p, std::size_t n) noexcept {}

    int x = 0;
};

auto intVec = MyDecorable::declareDecoration<std::vector<int>>();
auto str1 = MyDecorable::declareDecoration<std::string>();
auto str2 = MyDecorable::declareDecoration<std::string>();

constexpr auto testData = mongo::namespace_string_data::makeNsData<9, 4>("constexpr", "name");
constexpr mongo::NamespaceString kConstNs(testData.data(), testData.size());

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

    // Tests for various abseil containers.
    mongo::StringMap<int> emptyMap;

    mongo::StringMap<int> intMap;
    intMap["a"] = 1;
    intMap["b"] = 1;

    mongo::StringMap<std::string> strMap;
    strMap["a"] = "a_value";

    mongo::StringSet strSet;
    strSet.insert("a");

    absl::flat_hash_set<std::string, NonEmptyHash, mongo::StringMapEq> checkNonEmptyHash;
    absl::flat_hash_set<std::string, mongo::StringMapHasher, NonEmptyMapEq> checkNonEmptyEq;
    absl::flat_hash_set<std::string,
                        mongo::StringMapHasher,
                        mongo::StringMapEq,
                        NonEmptyAlloc<std::string>>
        checkNonEmptyAlloc;

    mongo::breakpoint();

    return 0;
}

#pragma GCC pop_options
