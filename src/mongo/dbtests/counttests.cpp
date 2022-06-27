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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"

namespace CountTests {

class Base {
public:
    Base() : _lk(&_opCtx, nss().dbName(), MODE_X), _context(&_opCtx, ns()), _client(&_opCtx) {
        _database = _context.db();

        {
            WriteUnitOfWork wunit(&_opCtx);

            auto collection =
                CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespaceForMetadataWrite(
                    &_opCtx, nss());
            if (collection) {
                _database->dropCollection(&_opCtx, nss()).transitional_ignore();
            }
            collection = _database->createCollection(&_opCtx, nss());

            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            auto indexSpec = BSON("v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion)
                                      << "key" << BSON("a" << 1) << "name"
                                      << "a_1");
            uassertStatusOK(
                indexCatalog->createIndexOnEmptyCollection(&_opCtx, collection, indexSpec));

            wunit.commit();

            _collection = collection;
        }
    }

    ~Base() {
        try {
            WriteUnitOfWork wunit(&_opCtx);
            uassertStatusOK(_database->dropCollection(&_opCtx, nss()));
            wunit.commit();
        } catch (...) {
            FAIL("Exception while cleaning up collection");
        }
    }

protected:
    static const char* ns() {
        return "unittests.counttests";
    }

    static NamespaceString nss() {
        return NamespaceString(ns());
    }

    void insert(const char* s) {
        WriteUnitOfWork wunit(&_opCtx);
        const BSONObj o = fromjson(s);
        OpDebug* const nullOpDebug = nullptr;

        if (o["_id"].eoo()) {
            BSONObjBuilder b;
            OID oid;
            oid.init();
            b.appendOID("_id", &oid);
            b.appendElements(o);
            _collection->insertDocument(&_opCtx, InsertStatement(b.obj()), nullOpDebug, false)
                .transitional_ignore();
        } else {
            _collection->insertDocument(&_opCtx, InsertStatement(o), nullOpDebug, false)
                .transitional_ignore();
        }
        wunit.commit();
    }

    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::DBLock _lk;

    OldClientContext _context;

    Database* _database;
    CollectionPtr _collection;

    DBDirectClient _client;
};

class Basic : public Base {
public:
    void run() {
        insert("{\"a\":\"b\"}");
        insert("{\"c\":\"d\"}");
        ASSERT_EQUALS(2ULL, _client.count(nss(), fromjson("{}")));
    }
};

class Query : public Base {
public:
    void run() {
        insert("{\"a\":\"b\"}");
        insert("{\"a\":\"b\",\"x\":\"y\"}");
        insert("{\"a\":\"c\"}");
        ASSERT_EQUALS(2ULL, _client.count(nss(), fromjson("{\"a\":\"b\"}")));
    }
};

class QueryFields : public Base {
public:
    void run() {
        insert("{\"a\":\"b\"}");
        insert("{\"a\":\"c\"}");
        insert("{\"d\":\"e\"}");
        ASSERT_EQUALS(1ULL, _client.count(nss(), fromjson("{\"a\":\"b\"}")));
    }
};

class IndexedRegex : public Base {
public:
    void run() {
        insert("{\"a\":\"c\"}");
        insert("{\"a\":\"b\"}");
        insert("{\"a\":\"d\"}");
        ASSERT_EQUALS(1ULL, _client.count(nss(), fromjson("{\"a\":/^b/}")));
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("count") {}

    void setupTests() {
        add<Basic>();
        add<Query>();
        add<QueryFields>();
        add<IndexedRegex>();
    }
};

OldStyleSuiteInitializer<All> myall;

}  // namespace CountTests
