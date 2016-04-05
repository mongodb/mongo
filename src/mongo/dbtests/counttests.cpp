// counttests.cpp : count.{h,cpp} unit tests.

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/stdx/thread.h"

#include "mongo/dbtests/dbtests.h"

namespace CountTests {

class Base {
public:
    Base()
        : _scopedXact(&_txn, MODE_IX),
          _lk(_txn.lockState(), nsToDatabaseSubstring(ns()), MODE_X),
          _context(&_txn, ns()),
          _client(&_txn) {
        _database = _context.db();

        {
            WriteUnitOfWork wunit(&_txn);
            _collection = _database->getCollection(ns());
            if (_collection) {
                _database->dropCollection(&_txn, ns());
            }
            _collection = _database->createCollection(&_txn, ns());
            wunit.commit();
        }

        addIndex(fromjson("{\"a\":1}"));
    }
    ~Base() {
        try {
            WriteUnitOfWork wunit(&_txn);
            uassertStatusOK(_database->dropCollection(&_txn, ns()));
            wunit.commit();
        } catch (...) {
            FAIL("Exception while cleaning up collection");
        }
    }

protected:
    static const char* ns() {
        return "unittests.counttests";
    }

    void addIndex(const BSONObj& key) {
        Helpers::ensureIndex(&_txn,
                             _collection,
                             key,
                             /*unique=*/false,
                             /*name=*/key.firstElementFieldName());
    }

    void insert(const char* s) {
        WriteUnitOfWork wunit(&_txn);
        const BSONObj o = fromjson(s);
        OpDebug* const nullOpDebug = nullptr;

        if (o["_id"].eoo()) {
            BSONObjBuilder b;
            OID oid;
            oid.init();
            b.appendOID("_id", &oid);
            b.appendElements(o);
            _collection->insertDocument(&_txn, b.obj(), nullOpDebug, false);
        } else {
            _collection->insertDocument(&_txn, o, nullOpDebug, false);
        }
        wunit.commit();
    }


    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    ScopedTransaction _scopedXact;
    Lock::DBLock _lk;

    OldClientContext _context;

    Database* _database;
    Collection* _collection;

    DBDirectClient _client;
};

class Basic : public Base {
public:
    void run() {
        insert("{\"a\":\"b\"}");
        insert("{\"c\":\"d\"}");
        ASSERT_EQUALS(2ULL, _client.count(ns(), fromjson("{}")));
    }
};

class Query : public Base {
public:
    void run() {
        insert("{\"a\":\"b\"}");
        insert("{\"a\":\"b\",\"x\":\"y\"}");
        insert("{\"a\":\"c\"}");
        ASSERT_EQUALS(2ULL, _client.count(ns(), fromjson("{\"a\":\"b\"}")));
    }
};

class QueryFields : public Base {
public:
    void run() {
        insert("{\"a\":\"b\"}");
        insert("{\"a\":\"c\"}");
        insert("{\"d\":\"e\"}");
        ASSERT_EQUALS(1ULL, _client.count(ns(), fromjson("{\"a\":\"b\"}")));
    }
};

class IndexedRegex : public Base {
public:
    void run() {
        insert("{\"a\":\"c\"}");
        insert("{\"a\":\"b\"}");
        insert("{\"a\":\"d\"}");
        ASSERT_EQUALS(1ULL, _client.count(ns(), fromjson("{\"a\":/^b/}")));
    }
};


class All : public Suite {
public:
    All() : Suite("count") {}

    void setupTests() {
        add<Basic>();
        add<Query>();
        add<QueryFields>();
        add<IndexedRegex>();
    }
};

SuiteInstance<All> myall;

}  // namespace CountTests
