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

#include <map>
#include <ostream>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"


/**
 * The following facilitates writing tests in the Server Discovery And Monitoring (sdam) namespace.
 */
namespace mongo {
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& s) {
    os << "[";
    size_t i = 0;
    for (const auto& item : s) {
        os << item;
        if (i != s.size() - 1)
            os << ", ";
    }
    os << "]";
    return os;
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::set<T>& s) {
    os << "{";
    size_t i = 0;
    for (const auto& item : s) {
        os << item;
        if (i != s.size() - 1)
            os << ", ";
    }
    os << "}";
    return os;
}

template <typename K, typename V>
std::ostream& operator<<(std::ostream& os, const std::map<K, V>& m) {
    os << "{";
    size_t i = 0;
    for (const auto& item : m) {
        os << item.first << ": " << item.second;
        if (i != m.size() - 1)
            os << ", ";
    }
    os << "}";
    return os;
}

template std::ostream& operator<<(std::ostream& os,
                                  const std::vector<mongo::sdam::ServerDescriptionPtr>& v);
template std::ostream& operator<<(std::ostream& os, const std::set<std::string>& s);
template std::ostream& operator<<(std::ostream& os, const std::map<std::string, std::string>& m);
};  // namespace mongo

// We include this here because the ASSERT_EQUALS needs to have the operator<< defined
// beforehand for the types used in the tests.
#include "mongo/unittest/unittest.h"
namespace mongo {
namespace sdam {
using mongo::operator<<;

class SdamTestFixture : public mongo::unittest::Test {
protected:
    template <typename T, typename U>
    std::vector<U> map(std::vector<T> source, std::function<U(const T&)> f) {
        std::vector<U> result;
        std::transform(source.begin(),
                       source.end(),
                       std::back_inserter(result),
                       [f](const auto& item) { return f(item); });
        return result;
    }

    template <typename T, typename U>
    std::set<U> mapSet(std::vector<T> source, std::function<U(const T&)> f) {
        auto v = map<T, U>(source, f);
        std::set<U> result(v.begin(), v.end());
        return result;
    }
};
}  // namespace sdam
}  // namespace mongo
