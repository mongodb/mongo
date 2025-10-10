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

#include "mongo/db/index/multikey_paths.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/unittest/unittest.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::multikey_paths {
namespace {

TEST(MultikeyPaths, PrintEmptyPaths) {
    MultikeyPaths paths;
    ASSERT_EQ(toString(paths), "[]");
}

TEST(MultikeyPaths, PrintEmptySetPaths) {
    MultikeyPaths paths;
    paths.resize(1);
    ASSERT_EQ(toString(paths), "[{}]");
}

TEST(MultikeyPaths, PrintEmptySetsPaths) {
    MultikeyPaths paths;
    paths.resize(2);
    ASSERT_EQ(toString(paths), "[{},{}]");
}

TEST(MultikeyPaths, PrintNonEmptySetPaths) {
    MultikeyPaths paths;
    paths.resize(2);
    paths[1].insert(2);
    ASSERT_EQ(toString(paths), "[{},{2}]");
}

TEST(MultikeyPaths, PrintNonEmptySetsPaths) {
    MultikeyPaths paths;
    paths.resize(4);
    paths[1].insert(2);
    paths[3].insert(0);
    paths[3].insert(1);
    paths[3].insert(2);
    ASSERT_EQ(toString(paths), "[{},{2},{},{0,1,2}]");
}

TEST(MultikeyPaths, SerializeParseRoundTrip) {
    MultikeyPaths paths;
    ASSERT_EQ(parse(serialize({}, paths)), paths);

    paths.resize(1);
    ASSERT_EQ(parse(serialize(BSON("a" << 1), paths)), paths);

    paths.resize(2);
    ASSERT_EQ(parse(serialize(BSON("a" << 1 << "a.b.c" << 1), paths)), paths);

    paths[1].insert(2);
    ASSERT_EQ(parse(serialize(BSON("a" << 1 << "a.b.c" << 1), paths)), paths);

    paths.resize(4);
    paths[3].insert(0);
    paths[3].insert(1);
    paths[3].insert(2);
    ASSERT_EQ(parse(serialize(BSON("a" << 1 << "a.b.c" << 1 << "a.d.e.f" << 1 << "a.g.h.i.j" << 1),
                              paths)),
              paths);
}

TEST(MultikeyPaths, ParseInvalid) {
    ASSERT_EQ(parse(BSON("a" << 1)), ErrorCodes::BadValue);
    ASSERT_EQ(parse(BSON("a" << "str")), ErrorCodes::BadValue);

    std::string value(2049, 'a');
    BSONBinData binData{
        value.data(), static_cast<int>(value.length()), BinDataType::BinDataGeneral};
    ASSERT_EQ(parse(BSON("a" << binData)), ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo::multikey_paths
