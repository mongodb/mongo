// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_metadata_recoverer.h"

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace shard_catalog_recoverer {

// Forward declaration so we can call this under test.
bool isRecoveredShardVersionSufficient(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& installedShardVersion,
    bool isUnowned,
    const boost::optional<ChunkVersion>& optReceivedShardVersion);

}  // namespace shard_catalog_recoverer

namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");

class IsRecoveredShardVersionSufficientTest : public ServiceContextTest {
public:
    bool isSufficient(const ChunkVersion& installedShardVersion,
                      const boost::optional<ChunkVersion>& receivedShardVersion,
                      bool isUnowned = false) {
        return shard_catalog_recoverer::isRecoveredShardVersionSufficient(
            opCtx, kNss, installedShardVersion, isUnowned, receivedShardVersion);
    }

    ServiceContext::UniqueOperationContext opCtxHolder{makeOperationContext()};
    OperationContext* opCtx{opCtxHolder.get()};

    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};
    const OID otherEpoch{OID::gen()};
    const Timestamp otherTimestamp{2, 0};
};

TEST_F(IsRecoveredShardVersionSufficientTest, NoReceivedVersionIsAlwaysSufficient) {
    ASSERT_TRUE(isSufficient(ChunkVersion({epoch, timestamp}, {3, 1}), boost::none));
}

TEST_F(IsRecoveredShardVersionSufficientTest, IgnoredReceivedVersionIsSufficient) {
    ASSERT_TRUE(isSufficient(ChunkVersion({epoch, timestamp}, {3, 1}), ChunkVersion::IGNORED()));
}

TEST_F(IsRecoveredShardVersionSufficientTest, UnownedShardWithNoChunksReceivedIsSufficient) {
    // The generations deliberately differ: an UNOWNED shard cannot tell whether the collection is
    // tracked elsewhere, so any router view reporting no chunks here is compatible.
    ASSERT_TRUE(isSufficient(ChunkVersion::UNTRACKED(),
                             ChunkVersion({otherEpoch, otherTimestamp}, {0, 0}),
                             true /* isUnowned */));
}

TEST_F(IsRecoveredShardVersionSufficientTest, StaleRouterVersionIsSufficient) {
    ASSERT_TRUE(isSufficient(ChunkVersion({epoch, timestamp}, {5, 0}),
                             ChunkVersion({epoch, timestamp}, {3, 0})));
}

TEST_F(IsRecoveredShardVersionSufficientTest, NewerReceivedVersionIsNotSufficient) {
    ASSERT_FALSE(isSufficient(ChunkVersion({epoch, timestamp}, {3, 0}),
                              ChunkVersion({epoch, timestamp}, {5, 0})));
}

// Identical versions compare equivalent, so a router reporting exactly what this shard has
// installed must not be forced through the pessimistic configTime wait. Both cases below did take
// that wait back when <=> reported identical special/unset versions as unordered.
TEST_F(IsRecoveredShardVersionSufficientTest, MatchingUntrackedVersionsAreSufficient) {
    ASSERT_TRUE(isSufficient(ChunkVersion::UNTRACKED(), ChunkVersion::UNTRACKED()));
}

TEST_F(IsRecoveredShardVersionSufficientTest, MatchingTrackedZeroChunksVersionsAreSufficient) {
    ASSERT_TRUE(isSufficient(ChunkVersion({epoch, timestamp}, {0, 0}),
                             ChunkVersion({epoch, timestamp}, {0, 0})));
}

// Genuinely non-comparable versions must still fall through to the configTime wait, even when both
// sides report that this shard owns no chunks.
TEST_F(IsRecoveredShardVersionSufficientTest, UntrackedVersusTrackedZeroChunksIsNotSufficient) {
    ASSERT_FALSE(isSufficient(ChunkVersion::UNTRACKED(), ChunkVersion({epoch, timestamp}, {0, 0})));
    ASSERT_FALSE(isSufficient(ChunkVersion({epoch, timestamp}, {0, 0}), ChunkVersion::UNTRACKED()));
}

TEST_F(IsRecoveredShardVersionSufficientTest, UnsetAgainstSetSameGenerationIsNotSufficient) {
    ASSERT_FALSE(isSufficient(ChunkVersion({epoch, timestamp}, {0, 0}),
                              ChunkVersion({epoch, timestamp}, {3, 1})));
}

}  // namespace
}  // namespace mongo
