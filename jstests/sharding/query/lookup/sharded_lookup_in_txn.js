/**
 * Tests that $lookup and $graphLookup against a sharded collection are banned in a transaction.
 * @tags: [
 *   uses_transactions,
 * ]
 */
import {withAbortAndRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

(() => {
    const st = new ShardingTest({shards: 2, mongos: 1});
    const testName = "sharded_lookup_in_txn";

    const featureFlagAllowAdditionalParticipants = FeatureFlagUtil.isEnabled(
        st.configRS.getPrimary().getDB("admin"),
        "AllowAdditionalParticipants",
    );
    if (featureFlagAllowAdditionalParticipants) {
        jsTestLog(
            "Skipping as featureFlagAllowAdditionalParticipants is enabled, so $lookup against a sharded collection will not throw",
        );
        st.stop();
        return;
    }

    const mongosDB = st.s0.getDB(testName);
    assert.commandWorked(mongosDB.dropDatabase());

    // Shard the foreign collection on _id.
    const fromColl = mongosDB.from;
    st.shardColl(fromColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

    assert.commandWorked(fromColl.insert({_id: 0, b: 1}));
    assert.commandWorked(fromColl.insert({_id: 1, b: 2}));

    assert.commandWorked(mongosDB.local.insert({_id: 0, a: 1}));
    assert.commandWorked(mongosDB.local.insert({_id: 1, a: 2}));

    function assertFailsInTransaction(pipeline, errorCode) {
        const session = st.s.startSession();
        const sessionDB = session.getDatabase(testName);
        withAbortAndRetryOnTransientTxnError(session, () => {
            session.startTransaction();
            assert.throwsWithCode(() => sessionDB.local.aggregate(pipeline).itcount(), errorCode);
        });
        session.endSession();
    }

    // $lookup and $graphLookup against a sharded foreign collection in a transaction should fail.
    assertFailsInTransaction(
        [
            {
                $lookup: {
                    from: fromColl.getName(),
                    localField: "a",
                    foreignField: "b",
                    as: "res",
                },
            },
        ],
        28769,
    );

    assertFailsInTransaction(
        [
            {
                $graphLookup: {
                    from: fromColl.getName(),
                    startWith: "$a",
                    connectFromField: "b",
                    connectToField: "_id",
                    as: "res",
                },
            },
        ],
        28769,
    );

    st.stop();
})();
