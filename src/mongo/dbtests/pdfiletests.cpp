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

/**
 * pdfile unit tests
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/insert.h"
#include "mongo/dbtests/dbtests.h"

namespace PdfileTests {

namespace Insert {
class Base {
public:
    Base() : _lk(&_opCtx), _context(&_opCtx, nss()) {}

    virtual ~Base() {
        if (!collection())
            return;
        WriteUnitOfWork wunit(&_opCtx);
        _context.db()->dropCollection(&_opCtx, nss()).transitional_ignore();
        wunit.commit();
    }

protected:
    static NamespaceString nss() {
        return NamespaceString("unittests.pdfiletests.Insert");
    }
    CollectionPtr collection() {
        return CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss());
    }

    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::GlobalWrite _lk;
    OldClientContext _context;
};

class InsertNoId : public Base {
public:
    void run() {
        WriteUnitOfWork wunit(&_opCtx);
        BSONObj x = BSON("x" << 1);
        ASSERT(x["_id"].type() == 0);
        CollectionPtr coll =
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss());
        if (!coll) {
            coll = _context.db()->createCollection(&_opCtx, nss());
        }
        ASSERT(coll);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT(!coll->insertDocument(&_opCtx, InsertStatement(x), nullOpDebug, true).isOK());

        StatusWith<BSONObj> fixed = fixDocumentForInsert(&_opCtx, x);
        ASSERT(fixed.isOK());
        x = fixed.getValue();
        ASSERT(x["_id"].type() == jstOID);
        ASSERT(coll->insertDocument(&_opCtx, InsertStatement(x), nullOpDebug, true).isOK());
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

        BSONObj fixed = fixDocumentForInsert(&_opCtx, o).getValue();
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

        BSONObj fixed = fixDocumentForInsert(&_opCtx, o).getValue();
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
        ASSERT(fixDocumentForInsert(&_opCtx, BSON("_id" << 5)).isOK());
        ASSERT(fixDocumentForInsert(&_opCtx, BSON("_id" << BSON("x" << 5))).isOK());
        ASSERT(!fixDocumentForInsert(&_opCtx, BSON("_id" << BSON("$x" << 5))).isOK());
        ASSERT(!fixDocumentForInsert(&_opCtx, BSON("_id" << BSON("$oid" << 5))).isOK());
    }
};
}  // namespace Insert

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("pdfile") {}

    void setupTests() {
        add<Insert::InsertNoId>();
        add<Insert::UpdateDate>();
        add<Insert::UpdateDate2>();
        add<Insert::ValidId>();
    }
};

OldStyleSuiteInitializer<All> myall;

}  // namespace PdfileTests
