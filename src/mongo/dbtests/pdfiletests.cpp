// pdfiletests.cpp : pdfile unit tests.
//

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
#include "mongo/db/json.h"
#include "mongo/db/ops/insert.h"
#include "mongo/dbtests/dbtests.h"

namespace PdfileTests {

namespace Insert {
class Base {
public:
    Base() : _scopedXact(&_txn, MODE_X), _lk(_txn.lockState()), _context(&_txn, ns()) {}

    virtual ~Base() {
        if (!collection())
            return;
        WriteUnitOfWork wunit(&_txn);
        _context.db()->dropCollection(&_txn, ns());
        wunit.commit();
    }

protected:
    const char* ns() {
        return "unittests.pdfiletests.Insert";
    }
    Collection* collection() {
        return _context.db()->getCollection(ns());
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    ScopedTransaction _scopedXact;
    Lock::GlobalWrite _lk;
    OldClientContext _context;
};

class InsertNoId : public Base {
public:
    void run() {
        WriteUnitOfWork wunit(&_txn);
        BSONObj x = BSON("x" << 1);
        ASSERT(x["_id"].type() == 0);
        Collection* collection = _context.db()->getOrCreateCollection(&_txn, ns());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT(!collection->insertDocument(&_txn, x, nullOpDebug, true).isOK());

        StatusWith<BSONObj> fixed = fixDocumentForInsert(x);
        ASSERT(fixed.isOK());
        x = fixed.getValue();
        ASSERT(x["_id"].type() == jstOID);
        ASSERT(collection->insertDocument(&_txn, x, nullOpDebug, true).isOK());
        wunit.commit();
    }
};

class UpdateDate : public Base {
public:
    void run() {
        BSONObjBuilder b;
        b.appendTimestamp("a");
        b.append("_id", 1);
        BSONObj o = b.done();

        BSONObj fixed = fixDocumentForInsert(o).getValue();
        ASSERT_EQUALS(2, fixed.nFields());
        ASSERT(fixed.firstElement().fieldNameStringData() == "_id");
        ASSERT(fixed.firstElement().number() == 1);

        BSONElement a = fixed["a"];
        ASSERT(o["a"].type() == bsonTimestamp);
        ASSERT(o["a"].timestampValue() == 0);
        ASSERT(a.type() == bsonTimestamp);
        ASSERT(a.timestampValue() > 0);
    }
};

class UpdateDate2 : public Base {
public:
    void run() {
        BSONObj o;
        {
            BSONObjBuilder b;
            b.appendTimestamp("a");
            b.appendTimestamp("b");
            b.append("_id", 1);
            o = b.obj();
        }

        BSONObj fixed = fixDocumentForInsert(o).getValue();
        ASSERT_EQUALS(3, fixed.nFields());
        ASSERT(fixed.firstElement().fieldNameStringData() == "_id");
        ASSERT(fixed.firstElement().number() == 1);

        BSONElement a = fixed["a"];
        ASSERT(o["a"].type() == bsonTimestamp);
        ASSERT(o["a"].timestampValue() == 0);
        ASSERT(a.type() == bsonTimestamp);
        ASSERT(a.timestampValue() > 0);

        BSONElement b = fixed["b"];
        ASSERT(o["b"].type() == bsonTimestamp);
        ASSERT(o["b"].timestampValue() == 0);
        ASSERT(b.type() == bsonTimestamp);
        ASSERT(b.timestampValue() > 0);
    }
};

class ValidId : public Base {
public:
    void run() {
        ASSERT(fixDocumentForInsert(BSON("_id" << 5)).isOK());
        ASSERT(fixDocumentForInsert(BSON("_id" << BSON("x" << 5))).isOK());
        ASSERT(!fixDocumentForInsert(BSON("_id" << BSON("$x" << 5))).isOK());
        ASSERT(!fixDocumentForInsert(BSON("_id" << BSON("$oid" << 5))).isOK());
    }
};
}  // namespace Insert

class All : public Suite {
public:
    All() : Suite("pdfile") {}

    void setupTests() {
        add<Insert::InsertNoId>();
        add<Insert::UpdateDate>();
        add<Insert::UpdateDate2>();
        add<Insert::ValidId>();
    }
};

SuiteInstance<All> myall;

}  // namespace PdfileTests
