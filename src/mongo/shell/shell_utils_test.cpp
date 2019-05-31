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

#include "mongo/shell/shell_utils.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(ShellUtils, BalancedTest) {
    using shell_utils::isBalanced;
    struct {
        std::string in;
        bool isBalanced;
    } specs[] = {{"x = 5", true},
                 {"function(){}", true},
                 {"function(){\n}", true},
                 {"function(){", false},
                 {R"(x = "{";)", true},
                 {"// {", true},
                 {"// \n {", false},
                 {R"("//" {)", false},
                 {R"({x:/x\//})", true},
                 {R"({ \/// })", false},
                 {"x = 5 + y ", true},
                 {"x = ", false},
                 {"x = // hello", false},
                 {"x = 5 +", false},
                 {" x ++", true},
                 {"-- x", true},
                 {"a.", false},
                 {"a. ", false},
                 {"a.b", true},
                 // SERVER-5809 and related cases --
                 {R"(a = {s:"\""})", true},
                 {R"(db.test.save({s:"\""}))", true},
                 {R"(printjson(" \" "))", true},  //-- SERVER-8554
                 {R"(var a = "\\";)", true},
                 {R"(var a = ("\\") //")", true},
                 {R"(var a = ("\\") //\")", true},
                 {R"(var a = ("\\") //\")", true},
                 {R"(var a = ("\\") //)", true},
                 {R"(var a = ("\\"))", true},
                 {R"(var a = ("\\\""))", true},
                 {R"(var a = ("\\" //")", false},
                 {R"(var a = ("\\" //)", false},
                 {R"(var a = ("\\")", false}};
    for (const auto& spec : specs) {
        ASSERT_EQUALS(isBalanced(spec.in), spec.isBalanced);
    }
}

}  // namespace
}  // namespace mongo
