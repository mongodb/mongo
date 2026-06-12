/**
 * Tests that cross-shard (WouldChangeOwningShard) updates which could move a non-compliant
 * document past the upgrade-to-'constraint' validator scan are blocked while the scan is in
 * progress.
 *
 * This test pauses the scan on shard0 with a failpoint, attempts the WCOS update while the scan
 * is paused (with a shard-key filter and with an _id filter), and verifies the update fails and
 * the document does not move. After the scan resumes it finds the non-compliant document, so the
 * collMod itself must fail.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagConstraintValidationLevel,
 *   requires_sharding,
 *   uses_parallel_shell,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const validator = {a: {$exists: true}};

// Raised by the validator scan when it finds a non-compliant document.
const kViolatingValidatorErrorCode = 12370902;

let st;
let mongosDB;
let collName;
let nonCompliantDocId;

// Pauses collection scans on shard0, starts the upgrade-to-'constraint' collMod (whose validator
// scan pauses on shard0 before reaching the non-compliant doc), runs testFn while the scan is
// paused, then resumes the scan and joins the collMod. The resumed scan must find the
// non-compliant doc and fail the collMod with kViolatingValidatorErrorCode.
function runWhileValidatorScanPaused(testFn) {
    const fp = configureFailPoint(st.rs0.getPrimary(), "hangCollScanDoWork");

    const awaitCollMod = startParallelShell(
        funWithArgs(
            (dbName, collName, errorCode) => {
                assert.commandFailedWithCode(
                    db
                        .getSiblingDB(dbName)
                        .runCommand({collMod: collName, validationLevel: "constraint"}),
                    errorCode,
                );
            },
            dbName,
            collName,
            kViolatingValidatorErrorCode,
        ),
        st.s.port,
    );

    // Always resume the scan and join the collMod, even on assertion failure; a leaked failpoint
    // turns any failure in testFn into a teardown hang.
    try {
        fp.wait();

        // The failpoint pauses any collection scan on the node, so confirm via currentOp that
        // the paused operation is the validator scan on our collection before proceeding. The
        // scan work happens in a getMore (the aggregate returns its cursor with an empty first
        // batch), so match the originating command as well.
        assert.soon(() => {
            return (
                st.rs0
                    .getPrimary()
                    .getDB("admin")
                    .aggregate([
                        {$currentOp: {}},
                        {
                            $match: {
                                active: true,
                                ns: `${dbName}.${collName}`,
                                $or: [
                                    {"command.aggregate": collName},
                                    {"cursor.originatingCommand.aggregate": collName},
                                ],
                            },
                        },
                    ])
                    .toArray().length > 0
            );
        }, "expected the validator scan to be running on shard0");

        testFn();
    } finally {
        fp.off();
        awaitCollMod();
    }
}

// Asserts the non-compliant doc is still on shard0, and that the failed upgrade left
// validationLevel and the prepare flag unchanged. Must be called after the failpoint is off:
// these finds have no index on 'b', so their COLLSCAN would hit the failpoint and hang.
function assertStateUnchanged() {
    const onShard0 = st.shard0
        .getDB(dbName)
        [collName].find({b: "non-compliant, missing a"})
        .toArray();
    const onShard1 = st.shard1
        .getDB(dbName)
        [collName].find({b: "non-compliant, missing a"})
        .toArray();

    jsTest.log.info("Non-compliant doc location", {onShard0, onShard1});

    assert.eq(onShard0.length, 1, "non-compliant doc should still be on shard0", {
        onShard0,
        onShard1,
    });
    assert.eq(onShard1.length, 0, "non-compliant doc must not have moved to shard1", {
        onShard0,
        onShard1,
    });

    // Later test cases rely on the prepare flag still being active.
    const collInfo = mongosDB.getCollectionInfos({name: collName})[0].options;
    assert.eq(collInfo.validationLevel, "strict", "validationLevel must not have changed", {
        collInfo,
    });
    assert.eq(
        collInfo.prepareConstraintValidationLevel,
        true,
        "prepareConstraintValidationLevel must survive the failed upgrade",
        {collInfo},
    );
}

describe("cross-shard updates that could dodge the upgrade-to-constraint validator scan", function () {
    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 1}});

        collName = jsTestName();
        mongosDB = st.s.getDB(dbName);
        const coll = mongosDB[collName];

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(
            mongosDB.createCollection(collName, {validator, validationLevel: "strict"}),
        );
        // shardColl: shards on {x:1}, splits at x:5, moves [5,+inf) to shard1.
        st.shardColl(collName, {x: 1}, {x: 5}, {x: 5}, dbName, true);

        // Insert 10 compliant docs across both shards.
        for (let i = 0; i < 10; i++) {
            assert.commandWorked(coll.insertOne({x: i, a: 1}));
        }

        // Insert 1 non-compliant doc via bypass (while flag is not yet set).
        // x:-1 is distinct from all compliant docs and lands in shard0's chunk [-inf, 5).
        assert.commandWorked(
            mongosDB.runCommand({
                insert: collName,
                documents: [{x: -1, b: "non-compliant, missing a"}],
                bypassDocumentValidation: true,
            }),
        );

        nonCompliantDocId = coll.findOne({b: "non-compliant, missing a"})._id;

        // Set the prepare flag; it must stay set across the failed collMod attempts below so
        // each test case starts with it active.
        assert.commandWorked(
            mongosDB.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );

        jsTest.log.info("Setup complete", {
            shard0: st.shard0.shardName,
            shard1: st.shard1.shardName,
            nonCompliantDoc: {x: -1, _id: nonCompliantDocId, note: "missing 'a', lives on shard0"},
        });
    });

    after(function () {
        st.stop();
    });

    const wcosCases = [
        {name: "shard-key filter", filter: () => ({x: -1})},
        {name: "_id filter (write without shard key)", filter: () => ({_id: nonCompliantDocId})},
    ];

    for (const {name, filter} of wcosCases) {
        it(`${name}: WCOS update is blocked while the validator scan is paused`, function () {
            runWhileValidatorScanPaused(() => {
                const session = mongosDB.getMongo().startSession({retryWrites: true});
                let updateErr;
                try {
                    session.getDatabase(dbName)[collName].updateOne(filter(), {$set: {x: 7}});
                } catch (e) {
                    updateErr = e;
                }
                session.endSession();

                jsTest.log.info("Update error (if any)", {
                    updateErr: updateErr ? updateErr.message : null,
                });

                assert(
                    updateErr !== undefined,
                    "expected the cross-shard update to fail but it succeeded",
                );
                assert(
                    updateErr.message.includes("During insert stage of updating a shard key"),
                    "expected the update to fail at the WCOS re-insert",
                    {updateErr: updateErr.message},
                );
                assert(
                    updateErr.message.includes("Document failed validation"),
                    "expected the WCOS re-insert to fail document validation",
                    {updateErr: updateErr.message},
                );
            });

            assertStateUnchanged();
        });
    }
});
