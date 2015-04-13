/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/oid.h"
#include "mongo/base/status_with.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

    using namespace mongo;


    TEST(CollectionType, Empty) {
        StatusWith<CollectionType> status = CollectionType::fromBSON(BSONObj());
        ASSERT_FALSE(status.isOK());
    }

    TEST(CollectionType, Basic) {
        const OID oid = OID::gen();
        StatusWith<CollectionType> status = CollectionType::fromBSON(
                                                BSON(CollectionType::fullNs("db.coll") <<
                                                     CollectionType::epoch(oid) <<
                                                     CollectionType::updatedAt(1ULL) <<
                                                     CollectionType::keyPattern(BSON("a" << 1)) <<
                                                     CollectionType::unique(true)));
        ASSERT_TRUE(status.isOK());

        CollectionType coll = status.getValue();
        ASSERT_TRUE(coll.validate().isOK());
        ASSERT_EQUALS(coll.getNs(), "db.coll");
        ASSERT_EQUALS(coll.getEpoch(), oid);
        ASSERT_EQUALS(coll.getUpdatedAt(), 1ULL);
        ASSERT_EQUALS(coll.getKeyPattern(), BSON("a" << 1));
        ASSERT_EQUALS(coll.getUnique(), true);
        ASSERT_EQUALS(coll.getAllowBalance(), true);
        ASSERT_EQUALS(coll.getDropped(), false);
    }

    TEST(CollectionType, BadType) {
        const OID oid = OID::gen();
        StatusWith<CollectionType> status = CollectionType::fromBSON(
                                                BSON(CollectionType::fullNs() << 1 <<
                                                     CollectionType::epoch(oid) <<
                                                     CollectionType::updatedAt(1ULL) <<
                                                     CollectionType::keyPattern(BSON("a" << 1)) <<
                                                     CollectionType::unique(true)));

        ASSERT_FALSE(status.isOK());
    }

} // namespace
