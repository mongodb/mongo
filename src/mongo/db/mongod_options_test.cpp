/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/mongod_options.h"

#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/value.h"

#include <fmt/format.h>

namespace mongo {
namespace {

TEST(MongodGeneralOptionsTest, ValidateSecurityAuthorization) {
    moe::OptionSection options;
    ASSERT_OK(addMongodOptions(&options));
    for (auto&& [in, ok] : std::vector<std::pair<std::string, bool>>{
             {"enabled", 1},
             {"disabled", 1},
             {"Enabled", 0},
             {"ENABLED", 0},
             {"Disabled", 0},
             {"DISABLED", 0},
             {"", 0},
             {"garbage", 0},
             {"Garbage", 0},
             {"GARBAGE", 0},
         }) {
        std::string yml = fmt::format(R"(security.authorization: "{}")", in);
        moe::Environment env;
        ASSERT_OK(moe::OptionsParser{}.runConfigFile(options, yml, &env));
        ASSERT_EQ(env.validate(false), ok ? ErrorCodes::OK : ErrorCodes::BadValue)
            << fmt::format(", in={}", in);
    }
}

}  // namespace
}  // namespace mongo
