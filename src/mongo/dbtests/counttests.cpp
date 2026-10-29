// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

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
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(&_opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
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

        if (o["_id"].eoo()) {
            BSONObjBuilder b;
            OID oid;
            oid.init();
            b.appendOID("_id", &oid);
            b.appendElements(o);
            Helpers::insert(&_opCtx, _collection->getCollectionPtr(), b.obj())
                .transitional_ignore();
        } else {
            Helpers::insert(&_opCtx, _collection->getCollectionPtr(), o).transitional_ignore();
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
