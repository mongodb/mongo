/**
 *    Copyright (C) 2015 10gen Inc.
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

#include <cstdint>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/service_context.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/dbtests/dbtests.h"

namespace ValidateTests {

using std::unique_ptr;

static const char* const _ns = "unittests.validate_tests";

/**
 * Test fixture for a write locked test using collection _ns.  Includes functionality to
 * partially construct a new IndexDetails in a manner that supports proper cleanup in
 * dropCollection().
 */
class ValidateBase {
public:
    explicit ValidateBase(bool full) : _ctx(&_txn, _ns), _client(&_txn), _full(full) {
        _client.createCollection(_ns);
    }
    ~ValidateBase() {
        _client.dropCollection(_ns);
        getGlobalServiceContext()->unsetKillAllOperations();
    }
    Collection* collection() {
        return _ctx.getCollection();
    }

protected:
    bool checkValid() {
        ValidateResults results;
        BSONObjBuilder output;
        ASSERT_OK(collection()->validate(&_txn, _full, false, &results, &output));

        //  Check if errors are reported if and only if valid is set to false.
        ASSERT_EQ(results.valid, results.errors.empty());

        return results.valid;
    }

    OperationContextImpl _txn;
    OldClientWriteContext _ctx;
    DBDirectClient _client;
    bool _full;
};

template <bool full>
class ValidateIdIndexCount : public ValidateBase {
public:
    ValidateIdIndexCount() : ValidateBase(full) {}

    void run() {
        // Create a new collection, insert records { _id: 1 } and {_id: 2} and check it's valid.
        Database* db = _ctx.db();
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_txn);
            ASSERT_OK(db->dropCollection(&_txn, _ns));
            coll = db->createCollection(&_txn, _ns);

            ASSERT_OK(coll->insertDocument(&_txn, BSON("_id" << 1), true));
            id1 = coll->getCursor(&_txn)->next()->id;
            ASSERT_OK(coll->insertDocument(&_txn, BSON("_id" << 2), true));
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());

        RecordStore* rs = coll->getRecordStore();

        // Remove a { _id: 1 } from the record store, so we get more _id entries than records, and
        // verify validate fails.
        {
            WriteUnitOfWork wunit(&_txn);
            rs->deleteRecord(&_txn, id1);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());

        // Insert records { _id: 0} and { _id: 1} , so we get too few _id entries, and verify
        // validate fails.
        {
            WriteUnitOfWork wunit(&_txn);
            for (int j = 0; j < 2; j++) {
                auto doc = BSON("_id" << j);
                ASSERT_OK(
                    rs->insertRecord(&_txn, doc.objdata(), doc.objsize(), /*enforceQuota*/ false));
            }
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());
    }
};

template <bool full>
class ValidateSecondaryIndexCount : public ValidateBase {
public:
    ValidateSecondaryIndexCount() : ValidateBase(full) {}
    void run() {
        // Create a new collection, insert two documents.
        Database* db = _ctx.db();
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_txn);
            ASSERT_OK(db->dropCollection(&_txn, _ns));
            coll = db->createCollection(&_txn, _ns);
            ASSERT_OK(coll->insertDocument(&_txn, BSON("_id" << 1 << "a" << 1), true));
            id1 = coll->getCursor(&_txn)->next()->id;
            ASSERT_OK(coll->insertDocument(&_txn, BSON("_id" << 2 << "a" << 2), true));
            wunit.commit();
        }

        dbtests::createIndex(&_txn,
                             coll->ns().ns(),
                             BSON("name"
                                  << "a"
                                  << "ns" << coll->ns().ns() << "key" << BSON("a" << 1)
                                  << "background" << false));

        ASSERT_TRUE(checkValid());

        RecordStore* rs = coll->getRecordStore();

        // Remove a record, so we get more _id entries than records, and verify validate fails.
        {
            WriteUnitOfWork wunit(&_txn);
            rs->deleteRecord(&_txn, id1);
                wunit.commit();
        }

        ASSERT_FALSE(checkValid());

        // Insert two more records, so we get too few entries for a non-sparse index, and
        // verify validate fails.
        {
            WriteUnitOfWork wunit(&_txn);
            for (int j = 0; j < 2; j++) {
                auto doc = BSON("_id" << j);
                ASSERT_OK(
                    rs->insertRecord(&_txn, doc.objdata(), doc.objsize(), /*enforceQuota*/ false));
            }
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());
    }
};

class ValidateTests : public Suite {
public:
    ValidateTests() : Suite("validate_tests") {}

    void setupTests() {
        // Add tests for both full validate and non-full validate.
        add<ValidateIdIndexCount<true>>();
        add<ValidateIdIndexCount<false>>();
        add<ValidateSecondaryIndexCount<true>>();
        add<ValidateSecondaryIndexCount<false>>();
    }
} validateTests;
}  // namespace ValidateTests
