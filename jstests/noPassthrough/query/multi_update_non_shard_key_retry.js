/**
 * Verifies that the non-UWE batch write executor behaves correctly when an update
 * command with multiple non-shard key update operations need to retry operations,
 * after a shard returned an error response.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let iteration = 0;

const runTest = (useUnifiedWriteExecutor) => {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1},
        mongosOptions: {setParameter: {featureFlagUnifiedWriteExecutor: useUnifiedWriteExecutor}},
    });

    const db = st.s.getDB(jsTestName());
    const coll = db.coll;
    coll.drop();

    // Create a sharded collection and split it across the 2 shards.
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: coll.getFullName(),
            find: {x: 1},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    // Try different error codes.
    const errors = [
        {
            succeeds: true,
            code: ErrorCodes.ShardCannotRefreshDueToLocksHeld,
            errorExtraInfo: {nss: coll.getFullName()},
        },
        {
            succeeds: true,
            code: ErrorCodes.CannotImplicitlyCreateCollection,
            errorExtraInfo: {nss: coll.getFullName()},
        },
        {succeeds: false, code: ErrorCodes.NoSuchTransaction},
        {succeeds: false, code: ErrorCodes.BadValue},
    ];

    errors.forEach((error) => {
        jsTest.log.info(`Testing with unifiedWriteExecutor=${useUnifiedWriteExecutor}, errorCode=${error.code}`);

        // Create clean state for starting.
        coll.remove({});

        assert.commandWorked(coll.insert({_id: -1, x: -1})); // shard0
        assert.commandWorked(coll.insert({_id: 1, x: 1})); // shard1

        const generateFailPointData = () => {
            let data = {
                failCommands: ["update"],
                errorCode: error.code,
                failInternalCommands: true,
            };
            if (error.hasOwnProperty("errorExtraInfo")) {
                data.errorExtraInfo = error.errorExtraInfo;
            }
            return data;
        };

        // Enable fail point on shard0 to make the next update command fail.
        assert.commandWorked(
            st.shard0.adminCommand({
                configureFailPoint: "failCommand",
                mode: {times: 1},
                data: generateFailPointData(),
            }),
        );

        try {
            // Non-shard key updates need either txns or retryable writes.
            const session = st.s.startSession({retryWrites: true});

            // Run a multi-statement update command with 2 non-shard key updates.

            // mongos will send an "update" command to each shard. The update command sent to the
            // shards includes both update statements, i.e. [0, 1]. The update command on shard0
            // will run into the configured failure once. The update command on shard1 will succeed.
            // However, as shard0 returns an error for its update command (including both
            // statements), mongos will send the update commands to both shards again.
            const updateRes = db.runCommand({
                update: coll.getName(),
                updates: [
                    {q: {_id: -1}, u: {$set: {bumped: 1}}, multi: false},
                    {q: {_id: 1}, u: {$set: {bumped: 1}}, multi: false},
                ],
                ordered: false,
                lsid: session.getSessionId(),
                // txnNumber must be larger than any txnNumber used in this session.
                txnNumber: NumberLong(100 + iteration++),
            });

            if (error.succeeds) {
                assert.commandWorked(updateRes);
                assert.eq(2, updateRes.n);
                assert.eq(2, updateRes.nModified);

                const docs = coll.find({}).sort({_id: 1}).toArray();
                assert.eq(2, docs.length);
                assert.eq(
                    [
                        {_id: -1, x: -1, bumped: 1},
                        {_id: 1, x: 1, bumped: 1},
                    ],
                    docs,
                );
            } else {
                // Non-retryable errors are surfaced as per-statement write errors.
                assert.eq(1, updateRes.ok, tojson(updateRes));
                assert(
                    updateRes.writeErrors && updateRes.writeErrors.length > 0,
                    "Expected writeErrors but got none: " + tojson(updateRes),
                );
                updateRes.writeErrors.forEach((writeError) =>
                    assert.eq(error.code, writeError.code, tojson(updateRes)),
                );
            }

            session.endSession();
        } finally {
            assert.commandWorked(
                st.shard0.adminCommand({
                    configureFailPoint: "failCommand",
                    mode: "off",
                }),
            );
        }
    });

    st.stop();
};

runTest(true /* useUnifiedWriteExecutor */);
runTest(false /* useUnifiedWriteExecutor */);
