/**
 * Testing random migration failpoint
 * @tags: [
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  requires_sharding
 * ]
 */

// The mongod secondaries are set to priority 0 to prevent the primaries from stepping down during
// migrations on slow evergreen builders.
var st = new ShardingTest({
    shards: 2,
    other: {
        enableBalancer: true,
        configOptions: {
            setParameter: {
                "failpoint.balancerShouldReturnRandomMigrations": "{mode: 'alwaysOn'}",
            }
        }
    }
});

const dbName = "balancer_should_Return_random_migrations_failpoint";
const collName = "testColl";
const fullCollName = dbName + "." + collName;
const numDocuments = 25;
const testDB = st.s.getDB(dbName);

// Load the data as an (unsplitable) collection
let bulk = testDB[collName].initializeUnorderedBulkOp();
for (let i = 0; i < numDocuments; ++i) {
    bulk.insert({"Surname": "Smith", "Age": i});
}
assert.commandWorked(bulk.execute());

// Find in which shard is the collection at the current moment
function getDataShard(collName) {
    try {
        const docsS0 = st.rs0.getPrimary().getCollection(fullCollName).countDocuments({});
        const docsS1 = st.rs1.getPrimary().getCollection(fullCollName).countDocuments({});
        if (docsS0 == numDocuments)
            return st.rs0;
        else if (docsS1 == numDocuments)
            return st.rs1;
    } catch (e) {
        if (e.code != ErrorCodes.QueryPlanKilled) {
            throw e;
        }
    }
    // If we get here the collection might have been moved during the execution of the function
    // We should simply retry to get a new position
    return null;
}

var initialShard;
assert.soon(() => {
    initialShard = getDataShard(fullCollName);
    return initialShard != null;
});

// We expect that the balancer will eventually move the collection with random migrations to
// another shard. We check that all docs were moved at a given time
assert.soon(() => {
    const currentShard = getDataShard(fullCollName);
    if (currentShard != null && initialShard != currentShard) {
        return true;
    }
});

st.stop();
