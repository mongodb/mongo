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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/distinct_scan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

/**
 * This file tests db/exec/distinct.cpp
 */

namespace mongo {
namespace QueryStageDistinct {

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("unittests.QueryStageDistinct");

class DistinctBase {
public:
    DistinctBase()
        : _expCtx(ExpressionContextBuilder{}.opCtx(&_opCtx).ns(nss).build()), _client(&_opCtx) {}

    virtual ~DistinctBase() {
        _client.dropCollection(nss);
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns_forTest(), obj));
    }

    void insert(const BSONObj& obj) {
        _client.insert(nss, obj);
    }

    /**
     * Returns the projected value from the working set that would
     * be returned in the 'values' field of the distinct command result.
     * Limited to NumberInt BSON types because this is the only
     * BSON type used in this suite of tests.
     */
    static int getIntFieldDotted(const WorkingSet& ws,
                                 WorkingSetID wsid,
                                 const std::string& field) {
        // For some reason (at least under OS X clang), we cannot refer to INVALID_ID
        // inside the test assertion macro.
        WorkingSetID invalid = WorkingSet::INVALID_ID;
        ASSERT_NOT_EQUALS(invalid, wsid);

        auto member = ws.get(wsid);

        // Distinct hack execution is always covered.
        // Key value is retrieved from working set key data
        // instead of RecordId.
        ASSERT_FALSE(member->hasObj());
        BSONElement keyElt;
        ASSERT_TRUE(member->getFieldDotted(field, &keyElt));
        ASSERT_TRUE(keyElt.isNumber());

        return keyElt.numberInt();
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    boost::intrusive_ptr<ExpressionContext> _expCtx;

private:
    DBDirectClient _client;
};

CollectionAcquisition acquireCollForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

// Tests distinct with single key indices.
class QueryStageDistinctBasic : public DistinctBase {
public:
    ~QueryStageDistinctBasic() override {}

    void run() {
        // Insert a ton of documents with a: 1
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 1));
        }

        // Insert a ton of other documents with a: 2
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 2));
        }

        // Make an index on a:1
        addIndex(BSON("a" << 1));

        const auto collection = acquireCollForRead(&_opCtx, nss);
        const CollectionPtr& collPtr = collection.getCollectionPtr();

        // Set up the distinct stage.
        std::vector<const IndexDescriptor*> indexes;
        collPtr->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, BSON("a" << 1), IndexCatalog::InclusionPolicy::kReady, &indexes);
        ASSERT_EQ(indexes.size(), 1U);

        DistinctParams params{&_opCtx, collPtr, indexes[0]};
        params.scanDirection = 1;
        // Distinct-ing over the 0-th field of the keypattern.
        params.fieldNo = 0;
        // We'll look at all values in the bounds.
        params.bounds.isSimpleRange = false;
        OrderedIntervalList oil("a");
        oil.intervals.push_back(IndexBoundsBuilder::allValues());
        params.bounds.fields.push_back(oil);

        WorkingSet ws;
        DistinctScan distinct(_expCtx.get(), collection, std::move(params), &ws);

        WorkingSetID wsid;
        // Get our first result.
        int firstResultWorks = 0;
        while (PlanStage::ADVANCED != distinct.work(&wsid)) {
            ++firstResultWorks;
        }
        // 5 is a bogus number.  There's some amount of setup done by the first few calls but
        // we should return the first result relatively promptly.
        ASSERT_LESS_THAN(firstResultWorks, 5);
        ASSERT_EQUALS(1, getIntFieldDotted(ws, wsid, "a"));

        // Getting our second result should be very quick as we just skip
        // over the first result.
        int secondResultWorks = 0;
        while (PlanStage::ADVANCED != distinct.work(&wsid)) {
            ++secondResultWorks;
        }
        ASSERT_EQUALS(2, getIntFieldDotted(ws, wsid, "a"));
        // This is 0 because we don't have to loop for several values; we just skip over
        // all the 'a' values.
        ASSERT_EQUALS(0, secondResultWorks);

        ASSERT_EQUALS(PlanStage::IS_EOF, distinct.work(&wsid));
    }
};

// Tests distinct with multikey indices.
class QueryStageDistinctMultiKey : public DistinctBase {
public:
    ~QueryStageDistinctMultiKey() override {}

    void run() {
        // Insert a ton of documents with a: [1, 2, 3]
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << BSON_ARRAY(1 << 2 << 3)));
        }

        // Insert a ton of other documents with a: [4, 5, 6]
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << BSON_ARRAY(4 << 5 << 6)));
        }

        // Make an index on a:1
        addIndex(BSON("a" << 1));

        const auto coll = acquireCollForRead(&_opCtx, nss);

        // Set up the distinct stage.
        std::vector<const IndexDescriptor*> indexes;
        coll.getCollectionPtr()->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, BSON("a" << 1), IndexCatalog::InclusionPolicy::kReady, &indexes);
        MONGO_verify(indexes.size() == 1);

        DistinctParams params{&_opCtx, coll.getCollectionPtr(), indexes[0]};
        ASSERT_TRUE(params.isMultiKey);

        params.scanDirection = 1;
        // Distinct-ing over the 0-th field of the keypattern.
        params.fieldNo = 0;
        // We'll look at all values in the bounds.
        params.bounds.isSimpleRange = false;
        OrderedIntervalList oil("a");
        oil.intervals.push_back(IndexBoundsBuilder::allValues());
        params.bounds.fields.push_back(oil);

        WorkingSet ws;
        DistinctScan distinct(_expCtx.get(), coll, std::move(params), &ws);

        // We should see each number in the range [1, 6] exactly once.
        std::set<int> seen;

        WorkingSetID wsid;
        PlanStage::StageState state;
        while (PlanStage::IS_EOF != (state = distinct.work(&wsid))) {
            if (PlanStage::ADVANCED == state) {
                // Check int value.
                int currentNumber = getIntFieldDotted(ws, wsid, "a");
                ASSERT_GREATER_THAN_OR_EQUALS(currentNumber, 1);
                ASSERT_LESS_THAN_OR_EQUALS(currentNumber, 6);

                // Should see this number only once.
                ASSERT_TRUE(seen.find(currentNumber) == seen.end());
                seen.insert(currentNumber);
            }
        }

        ASSERT_EQUALS(6U, seen.size());
    }
};

class QueryStageDistinctCompoundIndex : public DistinctBase {
public:
    void run() {
        // insert documents with a: 1 and b: 1
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 1 << "b" << 1));
        }
        // insert documents with a: 1 and b: 2
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 1 << "b" << 2));
        }
        // insert documents with a: 2 and b: 1
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 2 << "b" << 1));
        }
        // insert documents with a: 2 and b: 3
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 2 << "b" << 3));
        }

        addIndex(BSON("a" << 1 << "b" << 1));

        const auto coll = acquireCollForRead(&_opCtx, nss);

        std::vector<const IndexDescriptor*> indices;
        coll.getCollectionPtr()->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, BSON("a" << 1 << "b" << 1), IndexCatalog::InclusionPolicy::kReady, &indices);
        ASSERT_EQ(1U, indices.size());

        DistinctParams params{&_opCtx, coll.getCollectionPtr(), indices[0]};

        params.scanDirection = 1;
        params.fieldNo = 1;
        params.bounds.isSimpleRange = false;

        OrderedIntervalList aOil{"a"};
        aOil.intervals.push_back(IndexBoundsBuilder::allValues());
        params.bounds.fields.push_back(aOil);

        OrderedIntervalList bOil{"b"};
        bOil.intervals.push_back(IndexBoundsBuilder::allValues());
        params.bounds.fields.push_back(bOil);

        WorkingSet ws;
        DistinctScan distinct(_expCtx.get(), coll, std::move(params), &ws);

        WorkingSetID wsid;
        PlanStage::StageState state;

        std::vector<int> seen;

        while (PlanStage::IS_EOF != (state = distinct.work(&wsid))) {
            if (PlanStage::ADVANCED == state) {
                seen.push_back(getIntFieldDotted(ws, wsid, "b"));
            }
        }

        ASSERT_EQUALS(4U, seen.size());
        ASSERT_EQUALS(1, seen[0]);
        ASSERT_EQUALS(2, seen[1]);
        ASSERT_EQUALS(1, seen[2]);
        ASSERT_EQUALS(3, seen[3]);
    }
};

// XXX: add a test case with bounds where skipping to the next key gets us a result that's not
// valid w.r.t. our query.

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_distinct") {}

    void setupTests() override {
        add<QueryStageDistinctBasic>();
        add<QueryStageDistinctMultiKey>();
        add<QueryStageDistinctCompoundIndex>();
    }
};

unittest::OldStyleSuiteInitializer<All> queryStageDistinctAll;

}  // namespace QueryStageDistinct
}  // namespace mongo
