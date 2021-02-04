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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

using namespace fmt::literals;

class ReshardingDonorRecipientCommonInternalsTest : public ShardServerTestFixture {
public:
    const UUID kExistingUUID = UUID::gen();
    const NamespaceString kOriginalNss = NamespaceString("db", "foo");

    const NamespaceString kTemporaryReshardingNss =
        constructTemporaryReshardingNss("db", kExistingUUID);
    const std::string kOriginalShardKey = "oldKey";
    const BSONObj kOriginalShardKeyPattern = BSON(kOriginalShardKey << 1);
    const std::string kReshardingKey = "newKey";
    const BSONObj kReshardingKeyPattern = BSON(kReshardingKey << 1);
    const OID kOriginalEpoch = OID::gen();
    const OID kReshardingEpoch = OID::gen();
    const UUID kReshardingUUID = UUID::gen();

    const ShardId kThisShard = ShardId("shardOne");
    const ShardId kOtherShard = ShardId("shardTwo");

    const std::vector<ShardId> kShardIds = {kThisShard, kOtherShard};

    const Timestamp kFetchTimestamp = Timestamp(1, 0);

protected:
    CollectionMetadata makeShardedMetadataForOriginalCollection(
        OperationContext* opCtx, const ShardId& shardThatChunkExistsOn) {
        return makeShardedMetadata(opCtx,
                                   kOriginalNss,
                                   kOriginalShardKey,
                                   kOriginalShardKeyPattern,
                                   kExistingUUID,
                                   kOriginalEpoch,
                                   shardThatChunkExistsOn);
    }

    CollectionMetadata makeShardedMetadataForTemporaryReshardingCollection(
        OperationContext* opCtx, const ShardId& shardThatChunkExistsOn) {
        return makeShardedMetadata(opCtx,
                                   kTemporaryReshardingNss,
                                   kReshardingKey,
                                   kReshardingKeyPattern,
                                   kReshardingUUID,
                                   kReshardingEpoch,
                                   shardThatChunkExistsOn);
    }

    CollectionMetadata makeShardedMetadata(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const std::string& shardKey,
                                           const BSONObj& shardKeyPattern,
                                           const UUID& uuid,
                                           const OID& epoch,
                                           const ShardId& shardThatChunkExistsOn) {
        auto range = ChunkRange(BSON(shardKey << MINKEY), BSON(shardKey << MAXKEY));
        auto chunk = ChunkType(
            nss, std::move(range), ChunkVersion(1, 0, epoch, boost::none), shardThatChunkExistsOn);
        ChunkManager cm(
            kThisShard,
            DatabaseVersion(uuid),
            makeStandaloneRoutingTableHistory(RoutingTableHistory::makeNew(nss,
                                                                           uuid,
                                                                           shardKeyPattern,
                                                                           nullptr,
                                                                           false,
                                                                           epoch,
                                                                           boost::none,
                                                                           boost::none,
                                                                           true,
                                                                           {std::move(chunk)})),
            boost::none);

        if (!OperationShardingState::isOperationVersioned(opCtx)) {
            const auto version = cm.getVersion(kThisShard);
            BSONObjBuilder builder;
            version.appendToCommand(&builder);

            auto& oss = OperationShardingState::get(opCtx);
            oss.initializeClientRoutingVersionsFromCommand(nss, builder.obj());
        }

        return CollectionMetadata(std::move(cm), kThisShard);
    }

    ReshardingFields createCommonReshardingFields(const UUID& reshardingUUID,
                                                  CoordinatorStateEnum state) {
        auto fields = ReshardingFields(reshardingUUID);
        fields.setState(state);
        return fields;
    };

    void appendDonorFieldsToReshardingFields(ReshardingFields& fields,
                                             const BSONObj& reshardingKey) {
        fields.setDonorFields(TypeCollectionDonorFields(reshardingKey));
    }

    void appendRecipientFieldsToReshardingFields(
        ReshardingFields& fields,
        const std::vector<ShardId> donorShardIds,
        const UUID& existingUUID,
        const NamespaceString& originalNss,
        const boost::optional<Timestamp>& fetchTimestamp = boost::none) {
        auto recipientFields =
            TypeCollectionRecipientFields(donorShardIds, existingUUID, originalNss);
        emplaceFetchTimestampIfExists(recipientFields, fetchTimestamp);
        fields.setRecipientFields(std::move(recipientFields));
    }

    template <class ReshardingDocument>
    void assertCommonDocFieldsMatchReshardingFields(const NamespaceString& nss,
                                                    const UUID& reshardingUUID,
                                                    const UUID& existingUUID,
                                                    const BSONObj& reshardingKey,
                                                    const ReshardingDocument& reshardingDoc) {
        ASSERT_EQ(reshardingDoc.get_id(), reshardingUUID);
        ASSERT_EQ(reshardingDoc.getNss(), nss);
        ASSERT_EQ(reshardingDoc.getExistingUUID(), existingUUID);
        ASSERT_BSONOBJ_EQ(reshardingDoc.getReshardingKey().toBSON(), reshardingKey);
    }

    void assertDonorDocMatchesReshardingFields(const NamespaceString& nss,
                                               const UUID& existingUUID,
                                               const ReshardingFields& reshardingFields,
                                               const ReshardingDonorDocument& donorDoc) {
        assertCommonDocFieldsMatchReshardingFields<ReshardingDonorDocument>(
            nss,
            reshardingFields.getUuid(),
            existingUUID,
            reshardingFields.getDonorFields()->getReshardingKey().toBSON(),
            donorDoc);
        ASSERT(donorDoc.getState() == DonorStateEnum::kPreparingToDonate);
        ASSERT(donorDoc.getMinFetchTimestamp() == boost::none);
    }

    void assertRecipientDocMatchesReshardingFields(
        const CollectionMetadata& metadata,
        const ReshardingFields& reshardingFields,
        const ReshardingRecipientDocument& recipientDoc) {
        assertCommonDocFieldsMatchReshardingFields<ReshardingRecipientDocument>(
            reshardingFields.getRecipientFields()->getOriginalNamespace(),
            reshardingFields.getUuid(),
            reshardingFields.getRecipientFields()->getExistingUUID(),
            metadata.getShardKeyPattern().toBSON(),
            recipientDoc);

        ASSERT(recipientDoc.getState() == RecipientStateEnum::kCreatingCollection);
        ASSERT(recipientDoc.getFetchTimestamp() ==
               reshardingFields.getRecipientFields()->getFetchTimestamp());

        auto donorShardIds = reshardingFields.getRecipientFields()->getDonorShardIds();
        auto donorShardIdsSet = std::set<ShardId>(donorShardIds.begin(), donorShardIds.end());

        for (const auto& donorShardMirroringEntry : recipientDoc.getDonorShardsMirroring()) {
            ASSERT_EQ(donorShardMirroringEntry.getMirroring(), false);
            auto reshardingFieldsDonorShardId =
                donorShardIdsSet.find(donorShardMirroringEntry.getId());
            ASSERT(reshardingFieldsDonorShardId != donorShardIdsSet.end());
            donorShardIdsSet.erase(reshardingFieldsDonorShardId);
        }

        ASSERT(donorShardIdsSet.empty());
    }
};

class ReshardingDonorRecipientCommonTest : public ReshardingDonorRecipientCommonInternalsTest {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());

        std::unique_ptr<ReshardingDonorService> donorService =
            std::make_unique<ReshardingDonorService>(getServiceContext());
        _registry->registerService(std::move(donorService));

        std::unique_ptr<ReshardingRecipientService> recipientService =
            std::make_unique<ReshardingRecipientService>(getServiceContext());
        _registry->registerService(std::move(recipientService));
        _registry->onStartup(operationContext());

        stepUp();
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        Grid::get(operationContext())->getExecutorPool()->shutdownAndJoin();

        _registry->onShutdown();

        ShardServerTestFixture::tearDown();
    }

    void stepUp() {
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(operationContext(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            repl::OpTimeAndWallTime(repl::OpTime(Timestamp(1, 1), _term), Date_t()));

        _registry->onStepUpComplete(operationContext(), _term);
    }

protected:
    repl::PrimaryOnlyServiceRegistry* _registry;
    long long _term = 0;
};

}  // namespace

}  // namespace mongo
