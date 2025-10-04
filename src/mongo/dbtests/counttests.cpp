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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace CountTests {

class Base {
public:
    Base() : _autoDb(&_opCtx, nss().dbName(), MODE_X), _client(&_opCtx) {
        _database = _autoDb.ensureDbExists(&_opCtx);

        {
            WriteUnitOfWork wunit(&_opCtx);

            CollectionWriter writer{&_opCtx, nss()};
            auto collection = writer.getWritableCollection(&_opCtx);
            if (collection) {
                _database->dropCollection(&_opCtx, nss()).transitional_ignore();
            }
            collection = _database->createCollection(&_opCtx, nss());

            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            auto indexSpec = BSON("v" << static_cast<int>(IndexConfig::kLatestIndexVersion) << "key"
                                      << BSON("a" << 1) << "name"
                                      << "a_1");
            uassertStatusOK(
                indexCatalog->createIndexOnEmptyCollection(&_opCtx, collection, indexSpec));

            wunit.commit();
        }

        _collection = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest(nss(),
                                         PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                         repl::ReadConcernArgs::get(&_opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IS);
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
        return NamespaceString::createNamespaceString_forTest(ns());
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
            collection_internal::insertDocument(&_opCtx,
                                                _collection->getCollectionPtr(),
                                                InsertStatement(b.obj()),
                                                nullOpDebug,
                                                false)
                .transitional_ignore();
        } else {
            collection_internal::insertDocument(
                &_opCtx, _collection->getCollectionPtr(), InsertStatement(o), nullOpDebug, false)
                .transitional_ignore();
        }
        wunit.commit();
    }

    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    AutoGetDb _autoDb;

    Database* _database;
    boost::optional<CollectionAcquisition> _collection;

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

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("count") {}

    void setupTests() override {
        add<Basic>();
        add<Query>();
        add<QueryFields>();
        add<IndexedRegex>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace CountTests
}  // namespace mongo
