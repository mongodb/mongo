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

/**
 * This test fixture does not create any resharding POSs and should be preferred to
 * `ReshardingDonorRecipientCommonTest` when they are not required.
 */
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

    const DonorShardFetchTimestamp kThisShard =
        makeDonorShardFetchTimestamp(ShardId("shardOne"), Timestamp(10, 0));
    const DonorShardFetchTimestamp kOtherShard =
        makeDonorShardFetchTimestamp(ShardId("shardTwo"), Timestamp(20, 0));

    const std::vector<DonorShardFetchTimestamp> kShards = {kThisShard, kOtherShard};

    const Timestamp kCloneTimestamp = Timestamp(20, 0);

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
        ChunkManager cm(kThisShard.getShardId(),
                        DatabaseVersion(uuid),
                        makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(nss,
                                                         uuid,
                                                         shardKeyPattern,
                                                         nullptr,
                                                         false,
                                                         epoch,
                                                         boost::none,
                                                         boost::none /* timeseriesFields */,
                                                         boost::none,
                                                         true,
                                                         {std::move(chunk)})),
                        boost::none);

        if (!OperationShardingState::isOperationVersioned(opCtx)) {
            const auto version = cm.getVersion(kThisShard.getShardId());
            BSONObjBuilder builder;
            version.appendToCommand(&builder);

            auto& oss = OperationShardingState::get(opCtx);
            oss.initializeClientRoutingVersionsFromCommand(nss, builder.obj());
        }

        return CollectionMetadata(std::move(cm), kThisShard.getShardId());
    }

    ReshardingDonorDocument makeDonorStateDoc() {
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kPreparingToDonate);

        ReshardingDonorDocument doc(std::move(donorCtx),
                                    {kThisShard.getShardId(), kOtherShard.getShardId()});

        NamespaceString sourceNss = kOriginalNss;
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(), sourceNss, sourceUUID, kTemporaryReshardingNss, kReshardingKeyPattern);

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        return doc;
    }

    ReshardingRecipientDocument makeRecipientStateDoc() {
        RecipientShardContext recipCtx;
        recipCtx.setState(RecipientStateEnum::kCloning);

        ReshardingRecipientDocument doc(
            std::move(recipCtx), {kThisShard.getShardId(), kOtherShard.getShardId()}, 1000);

        NamespaceString sourceNss = kOriginalNss;
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(), sourceNss, sourceUUID, kTemporaryReshardingNss, kReshardingKeyPattern);

        doc.setCommonReshardingMetadata(std::move(commonMetadata));

        // A document in the cloning state requires a clone timestamp.
        doc.setCloneTimestamp(kCloneTimestamp);
        return doc;
    }

    ReshardingFields createCommonReshardingFields(const UUID& reshardingUUID,
                                                  CoordinatorStateEnum state) {
        auto fields = ReshardingFields(reshardingUUID);
        fields.setState(state);
        return fields;
    };

    void appendDonorFieldsToReshardingFields(ReshardingFields& fields,
                                             const BSONObj& reshardingKey) {
        std::vector<ShardId> donorShardIds;
        for (const auto& shard : kShards) {
            donorShardIds.emplace_back(shard.getShardId());
        }

        fields.setDonorFields(
            TypeCollectionDonorFields(kTemporaryReshardingNss, reshardingKey, donorShardIds));
    }

    void appendRecipientFieldsToReshardingFields(
        ReshardingFields& fields,
        const std::vector<DonorShardFetchTimestamp> donorShards,
        const UUID& existingUUID,
        const NamespaceString& originalNss,
        const boost::optional<Timestamp>& cloneTimestamp = boost::none) {
        auto recipientFields =
            TypeCollectionRecipientFields(donorShards, existingUUID, originalNss, 5000);
        emplaceCloneTimestampIfExists(recipientFields, cloneTimestamp);
        fields.setRecipientFields(std::move(recipientFields));
    }

    template <class ReshardingDocument>
    void assertCommonDocFieldsMatchReshardingFields(const NamespaceString& nss,
                                                    const UUID& reshardingUUID,
                                                    const UUID& existingUUID,
                                                    const BSONObj& reshardingKey,
                                                    const ReshardingDocument& reshardingDoc) {
        ASSERT_EQ(reshardingDoc.getReshardingUUID(), reshardingUUID);
        ASSERT_EQ(reshardingDoc.getSourceNss(), nss);
        ASSERT_EQ(reshardingDoc.getSourceUUID(), existingUUID);
        ASSERT_BSONOBJ_EQ(reshardingDoc.getReshardingKey().toBSON(), reshardingKey);
    }

    void assertDonorDocMatchesReshardingFields(const NamespaceString& nss,
                                               const UUID& existingUUID,
                                               const ReshardingFields& reshardingFields,
                                               const ReshardingDonorDocument& donorDoc) {
        assertCommonDocFieldsMatchReshardingFields<ReshardingDonorDocument>(
            nss,
            reshardingFields.getReshardingUUID(),
            existingUUID,
            reshardingFields.getDonorFields()->getReshardingKey().toBSON(),
            donorDoc);
        ASSERT(donorDoc.getMutableState().getState() == DonorStateEnum::kPreparingToDonate);
        ASSERT(donorDoc.getMutableState().getMinFetchTimestamp() == boost::none);
    }

    void assertRecipientDocMatchesReshardingFields(
        const CollectionMetadata& metadata,
        const ReshardingFields& reshardingFields,
        const ReshardingRecipientDocument& recipientDoc) {
        assertCommonDocFieldsMatchReshardingFields<ReshardingRecipientDocument>(
            reshardingFields.getRecipientFields()->getSourceNss(),
            reshardingFields.getReshardingUUID(),
            reshardingFields.getRecipientFields()->getSourceUUID(),
            metadata.getShardKeyPattern().toBSON(),
            recipientDoc);

        ASSERT(recipientDoc.getMutableState().getState() ==
               RecipientStateEnum::kAwaitingFetchTimestamp);
        ASSERT(!recipientDoc.getCloneTimestamp());

        const auto donorShards = reshardingFields.getRecipientFields()->getDonorShards();
        std::map<ShardId, DonorShardFetchTimestamp> donorShardMap;
        for (const auto& donor : donorShards) {
            donorShardMap.emplace(donor.getShardId(), donor);
        }

        for (const auto& donorShardFromRecipientDoc : recipientDoc.getDonorShards()) {
            auto donorIter = donorShardMap.find(donorShardFromRecipientDoc.getShardId());
            ASSERT(donorIter != donorShardMap.end());
            ASSERT_EQ(donorIter->second.getMinFetchTimestamp().has_value(),
                      donorShardFromRecipientDoc.getMinFetchTimestamp().has_value());

            if (donorIter->second.getMinFetchTimestamp()) {
                ASSERT_EQ(*donorIter->second.getMinFetchTimestamp(),
                          *donorShardFromRecipientDoc.getMinFetchTimestamp());
            }

            donorShardMap.erase(donorShardFromRecipientDoc.getShardId());
        }

        ASSERT(donorShardMap.empty());
    }

private:
    DonorShardFetchTimestamp makeDonorShardFetchTimestamp(
        ShardId shardId, boost::optional<Timestamp> fetchTimestamp) {
        DonorShardFetchTimestamp donorFetchTimestamp(shardId);
        donorFetchTimestamp.setMinFetchTimestamp(fetchTimestamp);
        return donorFetchTimestamp;
    }
};

/**
 * This fixture starts with the above internals test and also creates (notably) the resharding donor
 * and recipient POSs.
 */
class ReshardingDonorRecipientCommonTest : public ReshardingDonorRecipientCommonInternalsTest {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        _primaryOnlyServiceRegistry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());

        std::unique_ptr<ReshardingDonorService> donorService =
            std::make_unique<ReshardingDonorService>(getServiceContext());
        _primaryOnlyServiceRegistry->registerService(std::move(donorService));

        std::unique_ptr<ReshardingRecipientService> recipientService =
            std::make_unique<ReshardingRecipientService>(getServiceContext());
        _primaryOnlyServiceRegistry->registerService(std::move(recipientService));
        _primaryOnlyServiceRegistry->onStartup(operationContext());

        stepUp();
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        Grid::get(operationContext())->getExecutorPool()->shutdownAndJoin();

        _primaryOnlyServiceRegistry->onShutdown();

        Grid::get(operationContext())->clearForUnitTests();

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

        _primaryOnlyServiceRegistry->onStepUpComplete(operationContext(), _term);
    }

protected:
    repl::PrimaryOnlyServiceRegistry* _primaryOnlyServiceRegistry;
    long long _term = 0;
};

}  // namespace

}  // namespace mongo
