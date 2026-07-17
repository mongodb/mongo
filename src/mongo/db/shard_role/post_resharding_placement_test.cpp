// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/post_resharding_placement.h"

#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

// Minimal ScopedCollectionDescription::Impl backed by a caller-supplied CollectionMetadata. This
// lets us drive PostReshardingCollectionPlacement's construction-time validation without a live
// sharding environment.
class ScopedCollectionDescriptionMockImpl : public ScopedCollectionDescription::Impl {
public:
    explicit ScopedCollectionDescriptionMockImpl(CollectionMetadata metadata)
        : _metadata(std::move(metadata)) {}

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

ScopedCollectionDescription makeScopedCollectionDescription(CollectionMetadata metadata) {
    return ScopedCollectionDescription(
        std::make_shared<ScopedCollectionDescriptionMockImpl>(std::move(metadata)));
}

// A default-constructed (untracked) CollectionMetadata has no routing table, so
// getReshardingKeyIfShouldForwardOps() returns boost::none. This makes the constructed placement
// invalid (_invalidReason gets populated) and every accessor must trip the _checkIsValid() tassert
// (13150300) rather than dereferencing the never-populated members. opCtx is never touched: the
// constructor returns early once it sees no resharding key pattern.
//
// _checkIsValid() uses a tassert, which is a tripwire assertion that aborts the process, so each
// accessor is exercised in its own death test.

DEATH_TEST_REGEX(PostReshardingCollectionPlacementTest,
                 GetReshardingDestinedRecipientTassertsWhenInvalid,
                 "Tripwire assertion.*13150300") {
    PostReshardingCollectionPlacement placement(
        nullptr /* opCtx */, makeScopedCollectionDescription(CollectionMetadata::UNTRACKED()));
    placement.getReshardingDestinedRecipient(BSON("_id" << 1 << "x" << 5));
}

DEATH_TEST_REGEX(PostReshardingCollectionPlacementTest,
                 ExtractReshardingKeyFromDocumentTassertsWhenInvalid,
                 "Tripwire assertion.*13150300") {
    PostReshardingCollectionPlacement placement(
        nullptr /* opCtx */, makeScopedCollectionDescription(CollectionMetadata::UNTRACKED()));
    placement.extractReshardingKeyFromDocument(BSON("_id" << 1 << "x" << 5));
}

DEATH_TEST_REGEX(PostReshardingCollectionPlacementTest,
                 GetReshardingDestinedRecipientFromShardKeyTassertsWhenInvalid,
                 "Tripwire assertion.*13150300") {
    PostReshardingCollectionPlacement placement(
        nullptr /* opCtx */, makeScopedCollectionDescription(CollectionMetadata::UNTRACKED()));
    placement.getReshardingDestinedRecipientFromShardKey(BSON("x" << 5));
}

}  // namespace
}  // namespace mongo
