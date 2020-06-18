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

#include "mongo/platform/basic.h"

#include <iostream>
#include <string>

#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using namespace std::string_literals;

TEST(CstTest, BuildsAndPrints) {
    {
        const auto cst = CNode{CNode::Children{
            {KeyFieldname::atan2,
             CNode{CNode::Children{{KeyFieldname::arrayMarker, CNode{UserDouble{3.0}}},
                                   {KeyFieldname::arrayMarker, CNode{UserDouble{2.0}}}}}}}};
        ASSERT_EQ("{\natan2 :\n\t[\n\t\t<UserDouble 3.000000>\n\t\t<UserDouble 2.000000>\n\t]\n}"s,
                  cst.toString());
    }
    {
        const auto cst = CNode{CNode::Children{
            {KeyFieldname::project,
             CNode{CNode::Children{{UserFieldname{"a"}, CNode{KeyValue::trueKey}},
                                   {KeyFieldname::id, CNode{KeyValue::falseKey}}}}}}};
        ASSERT_EQ("{\nproject :\n\t{\n\ta :\n\t\t<KeyValue trueKey>\n"s +
                      "\tid :\n\t\t<KeyValue falseKey>\n\t}\n}",
                  cst.toString());
    }
}

}  // namespace
}  // namespace mongo
