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

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/bson/json.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata.h"
#include "mongo/unittest/unittest.h"

namespace {
using namespace mongo;
using namespace mongo::rpc;
using mongo::unittest::assertGet;

BSONObj addDollarDB(BSONObj command, StringData db) {
    return BSONObjBuilder(std::move(command)).append("$db", db).obj();
}

void checkUpconvert(const BSONObj& legacyCommand,
                    const int legacyQueryFlags,
                    BSONObj upconvertedCommand) {
    upconvertedCommand = addDollarDB(std::move(upconvertedCommand), "db");
    auto converted = upconvertRequest(DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                      legacyCommand,
                                      legacyQueryFlags);

    // We don't care about the order of the fields in the metadata object
    const auto sorted = [](const BSONObj& obj) {
        BSONObjIteratorSorted iter(obj);
        BSONObjBuilder bob;
        while (iter.more()) {
            bob.append(iter.next());
        }
        return bob.obj();
    };

    ASSERT_BSONOBJ_EQ(sorted(upconvertedCommand), sorted(converted.body));
}

TEST(Metadata, UpconvertValidMetadata) {
    // Unwrapped, no readPref, no slaveOk
    checkUpconvert(BSON("ping" << 1), 0, BSON("ping" << 1));

    // Readpref wrapped in $queryOptions
    checkUpconvert(BSON("pang"
                        << "pong"
                        << "$queryOptions"
                        << BSON("$readPreference" << BSON("mode"
                                                          << "nearest"
                                                          << "tags"
                                                          << BSON("rack"
                                                                  << "city")))),
                   0,
                   BSON("pang"
                        << "pong"
                        << "$readPreference"
                        << BSON("mode"
                                << "nearest"
                                << "tags"
                                << BSON("rack"
                                        << "city"))));
}

TEST(Metadata, UpconvertDuplicateReadPreference) {
    auto secondaryReadPref = BSON("mode"
                                  << "secondary");
    auto nearestReadPref = BSON("mode"
                                << "nearest");

    BSONObjBuilder bob;
    bob.append("$queryOptions", BSON("$readPreference" << secondaryReadPref));
    bob.append("$readPreference", nearestReadPref);

    ASSERT_THROWS_CODE(
        rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, "db"), bob.obj(), 0),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST(Metadata, UpconvertUsesDocumentSequecesCorrectly) {
    // These are cases where it is valid to use document sequences.
    const auto valid = {
        fromjson("{insert: 'coll', documents:[]}"),
        fromjson("{insert: 'coll', documents:[{a:1}]}"),
        fromjson("{insert: 'coll', documents:[{a:1}, {b:1}]}"),
    };

    // These are cases where it is not valid to use document sequences, but the command should still
    // be upconverted to the body.
    const auto invalid = {
        fromjson("{insert: 'coll'}"),
        fromjson("{insert: 'coll', documents:1}"),
        fromjson("{insert: 'coll', documents:[1]}"),
        fromjson("{insert: 'coll', documents:[{a:1}, 1]}"),
        fromjson("{NOT_insert: 'coll', documents:[{a:1}]}"),
    };

    for (const auto& cmd : valid) {
        const auto converted = rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, "db"), cmd, 0);
        ASSERT_BSONOBJ_EQ(converted.body, fromjson("{insert: 'coll', $db: 'db'}"));
        ASSERT_EQ(converted.sequences.size(), 1u);
        ASSERT_EQ(converted.sequences[0].name, "documents");

        std::vector<BSONObj> documents;
        for (auto&& elem : cmd["documents"].Obj()) {
            documents.push_back(elem.Obj());
        }

        ASSERT_EQ(converted.sequences[0].objs.size(), documents.size());
        for (size_t i = 0; i < documents.size(); i++) {
            ASSERT_BSONOBJ_EQ(converted.sequences[0].objs[i], documents[i]);

            // The documents should not be copied during upconversion. void* cast to prevent
            // treating pointers as c-strings when printing for failures.
            ASSERT_EQ(static_cast<const void*>(converted.sequences[0].objs[i].sharedBuffer().get()),
                      static_cast<const void*>(cmd.sharedBuffer().get()));
        }
    }

    for (const auto& cmd : invalid) {
        const auto converted = rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, "db"), cmd, 0);
        ASSERT_BSONOBJ_EQ(converted.body, addDollarDB(cmd, "db"));
        ASSERT_EQ(converted.sequences.size(), 0u);
    }
}

}  // namespace
