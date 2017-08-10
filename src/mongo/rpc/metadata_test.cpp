/*
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/client/dbclientinterface.h"
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
    auto converted = upconvertRequest("db", legacyCommand, legacyQueryFlags);

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
    // Wrapped in $query, with readPref and slaveOk bit set.
    checkUpconvert(BSON("$query" << BSON("ping" << 1) <<  //
                        "$readPreference"
                                 << BSON("mode"
                                         << "secondary")),
                   mongo::QueryOption_SlaveOk,
                   BSON("ping" << 1 << "$readPreference" << BSON("mode"
                                                                 << "secondary")));

    // Wrapped in 'query', with readPref.
    checkUpconvert(BSON("query" << BSON("pong" << 1 << "foo"
                                               << "bar")
                                << "$readPreference"
                                << BSON("mode"
                                        << "primary"
                                        << "tags"
                                        << BSON("dc"
                                                << "ny"))),
                   0,
                   BSON("pong" << 1 << "foo"
                               << "bar"
                               << "$readPreference"
                               << BSON("mode"
                                       << "primary"
                                       << "tags"
                                       << BSON("dc"
                                               << "ny"))));
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

TEST(Metadata, UpconvertInvalidMetadata) {
    // has $maxTimeMS option
    ASSERT_THROWS_CODE(upconvertRequest("db",
                                        BSON("query" << BSON("foo"
                                                             << "bar")
                                                     << "$maxTimeMS"
                                                     << 200),
                                        0),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(upconvertRequest("db",
                                        BSON("$query" << BSON("foo"
                                                              << "bar")
                                                      << "$maxTimeMS"
                                                      << 200),
                                        0),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    // invalid wrapped query
    ASSERT_THROWS(upconvertRequest("db", BSON("$query" << 1), 0), AssertionException);
    ASSERT_THROWS(upconvertRequest("db",
                                   BSON("$query"
                                        << ""),
                                   0),
                  AssertionException);
    ASSERT_THROWS(upconvertRequest("db", BSON("query" << 0), 0), AssertionException);
    ASSERT_THROWS(upconvertRequest("db",
                                   BSON("query"
                                        << ""),
                                   0),
                  AssertionException);
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

    for (auto cmd : valid) {
        const auto converted = rpc::upconvertRequest("db", cmd, 0);
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

    for (auto cmd : invalid) {
        const auto converted = rpc::upconvertRequest("db", cmd, 0);
        ASSERT_BSONOBJ_EQ(converted.body, addDollarDB(cmd, "db"));
        ASSERT_EQ(converted.sequences.size(), 0u);
    }
}

}  // namespace
