/**
 * Tests that write operations on the collection being unsharded succeed even when the routing
 * information for the associated temporary resharding collection isn't currently known.
 *
 * @tags: [
 *   uses_atclustertime,
 *   uses_transactions,
 *   requires_fcv_72,
 *   featureFlagReshardingImprovements,
 *   featureFlagUnshardCollection,
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   multiversion_incompatible
 * ]
 */
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const testCases = [
    {
        desc: "Test ordinary insert when donor does not have temporary resharding collection " +
            "routing info cached",
        ns: "unshardDb.coll_no_txn",
        opFn: (sourceCollection) => {
            const docToInsert = {_id: 0, oldKey: 5};
            assert.commandWorked(sourceCollection.insert(docToInsert));
            assert.eq(sourceCollection.findOne({_id: 0}), docToInsert);
        },
    },
    {
        desc: ("Test insert in a multi-statement transaction when donor does not have temporary " +
               "resharding collection routing info cached"),
        ns: "unshardDb.coll_in_txn_first_stmt",
        opFn: (sourceCollection) => {
            const mongos = sourceCollection.getMongo();
            const session = mongos.startSession();
            const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                                          .getCollection(sourceCollection.getName());

            const docToInsert = {_id: 0, oldKey: 5};
            session.startTransaction();
            assert.commandWorked(sessionCollection.insert(docToInsert));
            assert.commandWorked(session.commitTransaction_forTesting());
            assert.eq(sourceCollection.findOne({_id: 0}), docToInsert);
        },
    },
    {
        desc: ("Test insert in second statement of a multi-statement transaction when donor does " +
               "not have temporary resharding collection routing info cached"),
        ns: "unshardDb.coll_in_txn_second_stmt",
        opFn: (sourceCollection) => {
            const mongos = sourceCollection.getMongo();
            const session = mongos.startSession();
            const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                                          .getCollection(sourceCollection.getName());

            const sessionCollectionB =
                session.getDatabase(sourceCollection.getDB().getName()).getCollection('foo');
            assert.commandWorked(sessionCollectionB.insert({a: 1}));

            const docToInsert = {_id: 0, oldKey: 5};
            session.startTransaction();
            withTxnAndAutoRetryOnMongos(session, () => {
                assert.commandWorked(sessionCollectionB.insert({a: 2}));
                assert.commandWorked(sessionCollection.insert(docToInsert));
            });
            assert.commandWorked(session.commitTransaction_forTesting());
            assert.eq(sourceCollection.findOne({_id: 0}), docToInsert);
        },
    }
];

for (const {desc, ns, opFn} of testCases) {
    jsTest.log(desc);

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });

    const mongos = sourceCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);
    const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withUnshardCollectionInBackground(
        {toShard: recipientShardNames[0]}, (tempNs) => {
            // Wait for the recipients to have finished cloning so the temporary resharding
            // collection is known to exist.
            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.state === "applying";
            });

            // Make the routing info for the temporary sharding collection unknown
            donor0.adminCommand({flushRouterConfig: tempNs});

            opFn(sourceCollection);
        });
}

reshardingTest.teardown();
