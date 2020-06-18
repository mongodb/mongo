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

#include <numeric>
#include <type_traits>

#include "mongo/db/cst/c_node.h"
#include "mongo/util/visit_helper.h"

namespace mongo {
using namespace std::string_literals;
namespace {
auto tabs(int num) {
    std::string out;
    for (; num > 0; num--)
        out += "\t";
    return out;
}

auto printFieldname(const CNode::Fieldname& fieldname) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const KeyFieldname& key) -> std::string {
                return key_fieldname::toString[static_cast<std::underlying_type_t<KeyFieldname>>(
                    key)];
            },
            [](const UserFieldname& user) { return user; }},
        fieldname);
}

}  // namespace

std::string CNode::toStringHelper(int numTabs) const {
    return stdx::visit(
        visit_helper::Overloaded{
            [numTabs](const Children& children) {
                if (auto keyFieldname = stdx::get_if<KeyFieldname>(&children[0].first);
                    keyFieldname != nullptr && *keyFieldname == KeyFieldname::arrayMarker)
                    return std::accumulate(children.cbegin(),
                                           children.cend(),
                                           tabs(numTabs) + "[\n",
                                           [numTabs](auto&& string, auto&& childpair) {
                                               return string +
                                                   childpair.second.toStringHelper(numTabs + 1) +
                                                   "\n";
                                           }) +
                        tabs(numTabs) + "]";
                else
                    return std::accumulate(children.cbegin(),
                                           children.cend(),
                                           tabs(numTabs) + "{\n",
                                           [numTabs](auto&& string, auto&& childpair) {
                                               return string + tabs(numTabs) +
                                                   printFieldname(childpair.first) + " :\n" +
                                                   childpair.second.toStringHelper(numTabs + 1) +
                                                   "\n";
                                           }) +
                        tabs(numTabs) + "}";
            },
            [numTabs](const KeyValue& value) {
                return tabs(numTabs) + "<KeyValue " +
                    key_value::toString[static_cast<std::underlying_type_t<KeyValue>>(value)] + ">";
            },
            [numTabs](const UserDouble& userDouble) {
                return tabs(numTabs) + "<UserDouble " + std::to_string(userDouble) + ">";
            },
            [numTabs](const UserString& userString) {
                return tabs(numTabs) + "<UserString " + userString + ">";
            }},
        payload);
}

}  // namespace mongo
