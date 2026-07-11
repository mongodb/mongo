// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/insert.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo {
namespace PdfileTests {
namespace Insert {

class Base {
public:
    Base()
        : _lk(&_opCtx),
          _db(DatabaseHolder::get(&_opCtx)->openDb(&_opCtx, nss().dbName(), nullptr)) {}

    virtual ~Base() {
        if (!collection())
            return;
        WriteUnitOfWork wunit(&_opCtx);
        _db->dropCollection(&_opCtx, nss()).transitional_ignore();
        wunit.commit();
    }

protected:
    static NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest("unittests.pdfiletests.Insert");
    }
    CollectionPtr collection() {
        return CollectionPtr::CollectionPtr_UNSAFE(
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss()));
    }

    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::GlobalWrite _lk;
    Database* _db;
};

class InsertNoId : public Base {
public:
    void run() {
        WriteUnitOfWork wunit(&_opCtx);
        BSONObj x = BSON("x" << 1);
        ASSERT(stdx::to_underlying(x["_id"].type()) == 0);
        CollectionPtr coll = CollectionPtr::CollectionPtr_UNSAFE(
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss()));
        if (!coll) {
            coll = CollectionPtr::CollectionPtr_UNSAFE(_db->createCollection(&_opCtx, nss()));
        }
        ASSERT(coll);
        ASSERT_NOT_OK(Helpers::insert(&_opCtx, coll, x));

        StatusWith<BSONObj> fixed = fixDocumentForInsert(&_opCtx, x);
        ASSERT(fixed.isOK());
        x = fixed.getValue();
        ASSERT(x["_id"].type() == BSONType::oid);
        ASSERT_OK(Helpers::insert(&_opCtx, coll, x));
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
        ASSERT(o["a"].type() == BSONType::timestamp);
        ASSERT(o["a"].timestampValue() == 0);
        ASSERT(a.type() == BSONType::timestamp);
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
        ASSERT(o["a"].type() == BSONType::timestamp);
        ASSERT(o["a"].timestampValue() == 0);
        ASSERT(a.type() == BSONType::timestamp);
        ASSERT(a.timestampValue() > 0);

        BSONElement b = fixed["b"];
        ASSERT(o["b"].type() == BSONType::timestamp);
        ASSERT(o["b"].timestampValue() == 0);
        ASSERT(b.type() == BSONType::timestamp);
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

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("pdfile") {}

    void setupTests() override {
        add<Insert::InsertNoId>();
        add<Insert::UpdateDate>();
        add<Insert::UpdateDate2>();
        add<Insert::ValidId>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace PdfileTests
}  // namespace mongo
