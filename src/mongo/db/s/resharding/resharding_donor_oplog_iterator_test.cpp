/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include "mongo/logv2/log.h"

namespace mongo {
namespace {

repl::MutableOplogEntry makeOplog(const NamespaceString& nss,
                                  const UUID& uuid,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const ReshardingDonorOplogId& oplogId) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    oplogEntry.setOpTimeAndWallTimeBase(repl::OpTimeAndWallTimeBase({}, {}));
    oplogEntry.set_id(Value(oplogId.toBSON()));

    return oplogEntry;
}

class OnInsertAlwaysReady : public resharding::OnInsertAwaitable {
public:
    Future<void> awaitInsert(const ReshardingDonorOplogId& lastSeen) override {
        return Future<void>::makeReady();
    }
} onInsertAlwaysReady;

class ReshardingDonorOplogIterTest : public ShardingMongodTestFixture {
public:
    repl::MutableOplogEntry makeInsertOplog(const Timestamp& id, BSONObj doc) {
        ReshardingDonorOplogId oplogId(id, id);
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kInsert, BSON("x" << 1), {}, oplogId);
    }

    repl::MutableOplogEntry makeFinalOplog(const Timestamp& id) {
        ReshardingDonorOplogId oplogId(id, id);
        const BSONObj oField(BSON("msg"
                                  << "Created temporary resharding collection"));
        const BSONObj o2Field(BSON("type"
                                   << "reshardFinalOp"
                                   << "reshardingUUID" << UUID::gen()));
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kNoop, oField, o2Field, oplogId);
    }

    const NamespaceString& oplogNss() const {
        return _oplogNss;
    }

    BSONObj getId(const repl::MutableOplogEntry& oplog) {
        return oplog.get_id()->getDocument().toBson();
    }

    BSONObj getId(const repl::OplogEntry& oplog) {
        return oplog.get_id()->getDocument().toBson();
    }

private:
    const NamespaceString _oplogNss{"config.localReshardingOplogBuffer.xxx.yyy"};
    const NamespaceString _crudNss{"test.foo"};
    const UUID _uuid{UUID::gen()};
};

TEST_F(ReshardingDonorOplogIterTest, BasicExhaust) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));
    const auto oplogBeyond = makeInsertOplog(Timestamp(123, 46), BSON("z" << 1));

    DBDirectClient client(operationContext());
    const auto ns = oplogNss().ns();
    client.insert(ns, oplog1.toBSON());
    client.insert(ns, oplog2.toBSON());
    client.insert(ns, finalOplog.toBSON());
    client.insert(ns, oplogBeyond.toBSON());

    ReshardingDonorOplogIterator iter(oplogNss(), boost::none, &onInsertAlwaysReady);
    ASSERT_TRUE(iter.hasMore());
    auto next = iter.getNext(operationContext()).get();

    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(*next));

    ASSERT_TRUE(iter.hasMore());
    next = iter.getNext(operationContext()).get();
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(*next));

    ASSERT_TRUE(iter.hasMore());
    next = iter.getNext(operationContext()).get();
    ASSERT_FALSE(next);

    ASSERT_FALSE(iter.hasMore());
    next = iter.getNext(operationContext()).get();
    ASSERT_FALSE(next);
}

TEST_F(ReshardingDonorOplogIterTest, ResumeFromMiddle) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));

    DBDirectClient client(operationContext());
    const auto ns = oplogNss().ns();
    client.insert(ns, oplog1.toBSON());
    client.insert(ns, oplog2.toBSON());
    client.insert(ns, finalOplog.toBSON());

    ReshardingDonorOplogId resumeToken(Timestamp(2, 4), Timestamp(2, 4));
    ReshardingDonorOplogIterator iter(oplogNss(), resumeToken, &onInsertAlwaysReady);
    ASSERT_TRUE(iter.hasMore());
    auto next = iter.getNext(operationContext()).get();
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(*next));

    ASSERT_TRUE(iter.hasMore());
    next = iter.getNext(operationContext()).get();
    ASSERT_FALSE(next);

    ASSERT_FALSE(iter.hasMore());
}

TEST_F(ReshardingDonorOplogIterTest, ExhaustWithIncomingInserts) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));
    const auto oplogBeyond = makeInsertOplog(Timestamp(123, 46), BSON("z" << 1));

    DBDirectClient client(operationContext());
    const auto ns = oplogNss().ns();
    client.insert(ns, oplog1.toBSON());

    ReshardingDonorOplogIterator iter(oplogNss(), boost::none, &onInsertAlwaysReady);
    ASSERT_TRUE(iter.hasMore());
    auto next = iter.getNext(operationContext()).get();
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(*next));

    ASSERT_TRUE(iter.hasMore());

    client.insert(ns, oplog2.toBSON());
    client.insert(ns, finalOplog.toBSON());
    client.insert(ns, oplogBeyond.toBSON());

    next = iter.getNext(operationContext()).get();
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(*next));

    ASSERT_TRUE(iter.hasMore());
    next = iter.getNext(operationContext()).get();
    ASSERT_FALSE(next);

    ASSERT_FALSE(iter.hasMore());
    next = iter.getNext(operationContext()).get();
    ASSERT_FALSE(next);
}

}  // anonymous namespace
}  // namespace mongo
