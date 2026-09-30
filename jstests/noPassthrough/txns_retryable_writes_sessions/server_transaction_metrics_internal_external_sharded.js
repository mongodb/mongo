/**
 * Verifies serverStatus.transactions counts user-initiated transactions as external and
 * server-initiated ones as internal on a sharded cluster.
 *
 * Each shard is a 3-node replica set so that the participant-side prepare counters can be checked
 * on the secondaries as well as on the primaries.
 *
 * Cases:
 *   1. Single-shard user transaction
 *   2. Multi-shard user transaction
 *   3. Shard key change via retryable write (legacy WCOS path)
 *   4. Shard key change via plain update (transaction API WCOS path)
 *
 * @tags: [
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   requires_sharding,
 *   requires_fcv_91,
 *   resource_intensive,
 *   featureFlagServerInitiatedTransactionClassification,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "coll";
const ns = dbName + "." + collName;

// Router-side counters on mongos.
const routerFields = [
    "totalStarted",
    "totalCommitted",
    "totalAborted",
    "totalContactedParticipants",
];
const routerCommitTypes = [
    "singleShard",
    "singleWriteShard",
    "readOnly",
    "twoPhaseCommit",
    "twoPhaseCommitInternal",
    "twoPhaseCommitExternal",
    "recoverWithToken",
];
// Participant-side counters on each shard mongod.
const shardFields = [
    "totalStarted",
    "totalStartedInternal",
    "totalStartedExternal",
    "totalCommitted",
    "totalCommittedInternal",
    "totalCommittedExternal",
    "totalAborted",
    "totalAbortedInternal",
    "totalAbortedExternal",
    "totalPrepared",
    "totalPreparedInternal",
    "totalPreparedExternal",
    "totalPreparedThenCommitted",
    "totalPreparedThenCommittedInternal",
    "totalPreparedThenCommittedExternal",
    "totalPreparedThenAborted",
    "totalPreparedThenAbortedInternal",
    "totalPreparedThenAbortedExternal",
];
// Shard-side totals that must always equal their internal plus external halves.
const splitTotals = [
    "totalStarted",
    "totalCommitted",
    "totalAborted",
    "totalPrepared",
    "totalPreparedThenCommitted",
    "totalPreparedThenAborted",
];

/**
 * Marks an expected delta as a lower bound rather than an exact count.
 */
function atLeast(n) {
    return {atLeast: n};
}

/**
 * Extracts the expected delta for a specific field. If left out, the delta is 0.
 */
function getExpectedDelta(expectedDeltas, field) {
    if (expectedDeltas === undefined || expectedDeltas[field] === undefined) {
        return 0;
    }
    return expectedDeltas[field];
}

/**
 * Extracts a nested expectation, for example mongos' commitTypes or one shard's entry. If left out,
 * nothing changed, so hand back an empty expectation.
 */
function getSubExpectation(expected, key) {
    if (expected === undefined || expected[key] === undefined) {
        return {};
    }
    return expected[key];
}

/**
 * Sets up the sharded collection with one chunk on each of two shards: sk < 0 on shard0, sk >= 0 on
 * shard1.
 */
function setUpShardedCollection(shardingTest) {
    assert.commandWorked(
        shardingTest.s.adminCommand({
            enableSharding: dbName,
            primaryShard: shardingTest.shard0.shardName,
        }),
    );

    assert.commandWorked(shardingTest.s.adminCommand({shardCollection: ns, key: {sk: 1}}));
    assert.commandWorked(shardingTest.s.adminCommand({split: ns, middle: {sk: 0}}));
    assert.commandWorked(
        shardingTest.s.adminCommand({
            moveChunk: ns,
            find: {sk: 0},
            to: shardingTest.shard1.shardName,
        }),
    );
}

function getShards(shardingTest) {
    return [
        {name: shardingTest.shard0.shardName, rst: shardingTest.rs0},
        {name: shardingTest.shard1.shardName, rst: shardingTest.rs1},
    ];
}

function getTransactionsSection(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).transactions;
}

/**
 * Reads and logs the transactions section of mongos and of every mongod of every shard. Waits for
 * the secondaries to catch up first, so that their numbers reflect a fully-replicated state
 * rather than a race with oplog application.
 */
function pollAllNodes(shardingTest, label) {
    const mongosSection = getTransactionsSection(shardingTest.s);
    jsTest.log.info(`serverStatus.transactions [${label}] mongos ${shardingTest.s.host}`, {
        section: mongosSection,
    });

    const shardSections = getShards(shardingTest).map(({name, rst}) => {
        rst.awaitReplication();

        const primary = rst.getPrimary();
        const primarySection = getTransactionsSection(primary);
        jsTest.log.info(
            `serverStatus.transactions [${label}] shard ${name} primary ${primary.host}`,
            {section: primarySection},
        );

        const secondarySections = rst.getSecondaries().map((secondary) => {
            const section = getTransactionsSection(secondary);
            jsTest.log.info(
                `serverStatus.transactions [${label}] shard ${name} secondary ${secondary.host}`,
                {section},
            );
            return section;
        });

        return {primary: primarySection, secondaries: secondarySections};
    });

    return {mongos: mongosSection, shards: shardSections};
}

/**
 * Asserts that every counter in 'fields' moved by exactly the amount given in 'expectedDeltas'.
 *
 * An expectation is either a number, meaning the counter must have moved by exactly that much, or
 * atLeast(n), meaning it must have moved by at least that much. Fields omitted from
 * 'expectedDeltas' must not have moved at all.
 */
function assertFieldDeltas(fields, prevStats, currStats, expectedDeltas, description) {
    for (const field of fields) {
        const expected = getExpectedDelta(expectedDeltas, field);
        const actual = currStats[field] - prevStats[field];

        if (typeof expected === "number") {
            assert.eq(expected, actual, `unexpected delta for ${field} on ${description}`, {
                prevStats,
                currStats,
            });
        } else {
            assert.gte(
                actual,
                expected.atLeast,
                `delta for ${field} on ${description} is below the expected minimum`,
                {prevStats, currStats},
            );
        }
    }
}

/**
 * Asserts that each total still equals the sum of its internal and external halves. This has to
 * hold at any point in time, whatever the test just ran.
 */
function assertSplitTotals(currStats, description) {
    for (const total of splitTotals) {
        assert.eq(
            currStats[total],
            currStats[`${total}Internal`] + currStats[`${total}External`],
            `${total} split does not sum on ${description}`,
            {currStats},
        );
    }
}

/**
 * Asserts that no transaction is left open or prepared. Only shard mongods report
 * 'currentPrepared', so each field is checked where it is reported.
 */
function assertNoOpenTransactions(currStats, description) {
    for (const field of ["currentOpen", "currentPrepared"]) {
        if (currStats[field] !== undefined) {
            assert.eq(0, currStats[field], `unexpected ${field} on ${description}`, {currStats});
        }
    }
}

/**
 * Flattens the successful count out of each commit type so that commit types can be checked with
 * assertFieldDeltas like any other set of counters.
 */
function successfulCommitsByType(mongosStats) {
    const successes = {};
    for (const commitType of routerCommitTypes) {
        successes[commitType] = mongosStats.commitTypes[commitType].successful;
    }
    return successes;
}

function assertMongosDeltas(prevStats, currStats, expected) {
    assertFieldDeltas(routerFields, prevStats, currStats, expected, "mongos");
    assertFieldDeltas(
        routerCommitTypes,
        successfulCommitsByType(prevStats),
        successfulCommitsByType(currStats),
        getSubExpectation(expected, "commitTypes"),
        "mongos successful commits",
    );
    assertNoOpenTransactions(currStats, "mongos");
}

function assertShardNodeDeltas(prevStats, currStats, expectedDeltas, description) {
    assertFieldDeltas(shardFields, prevStats, currStats, expectedDeltas, description);
    assertSplitTotals(currStats, description);
    assertNoOpenTransactions(currStats, description);
}

/**
 * Asserts that mongos and every shard mongod moved by exactly the deltas in 'expected'; fields
 * omitted from 'expected' must not move. The 'secondaries' expectation applies to each secondary of
 * that shard individually. The shape of 'expected' is:
 *   {mongos: {..., commitTypes: {...}}, shards: [{primary: {...}, secondaries: {...}}, ...]}
 */
function assertDeltas(shardingTest, prev, curr, expected) {
    assertMongosDeltas(prev.mongos, curr.mongos, getSubExpectation(expected, "mongos"));

    getShards(shardingTest).forEach(({name, rst}, i) => {
        const expectedShard = getSubExpectation(expected.shards, i);

        assertShardNodeDeltas(
            prev.shards[i].primary,
            curr.shards[i].primary,
            expectedShard.primary,
            `${name} primary`,
        );

        const secondaryHosts = rst.getSecondaries().map((secondary) => secondary.host);
        curr.shards[i].secondaries.forEach((currStats, j) => {
            assertShardNodeDeltas(
                prev.shards[i].secondaries[j],
                currStats,
                expectedShard.secondaries,
                `${name} secondary ${secondaryHosts[j]}`,
            );
        });
    });
}

describe("serverStatus.transactions internal/external classification on a sharded cluster", function () {
    // ShardingTest 'st' exercises the default (legacy) WouldChangeOwningShardError (WCOS) path,
    // while 'st2' enables featureFlagUpdateDocumentShardKeyUsingTransactionApi so WCOS uses the
    // transaction API path instead.
    let st;
    let st2;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 3}});
        setUpShardedCollection(st);
        pollAllNodes(st, "baseline, before any transaction");

        const setParameter = {featureFlagUpdateDocumentShardKeyUsingTransactionApi: true};
        st2 = new ShardingTest({
            name: jsTestName() + "_txnApi",
            shards: 2,
            rs: {nodes: 3},
            rsOptions: {setParameter},
            mongosOptions: {setParameter},
            configOptions: {setParameter},
        });
        setUpShardedCollection(st2);
        pollAllNodes(st2, "baseline, before any transaction (txn API cluster)");
    });

    after(function () {
        st.stop();
        st2.stop();
    });

    // Every statement targets a negative sk, so shard0 is the only participant and mongos can
    // commit with the single-shard protocol rather than two-phase commit. Because the transaction
    // is never prepared, it replicates as a single applyOps entry that does not go through a
    // TransactionParticipant, so the secondaries count nothing at all.
    it("case 1: counts a single-shard transaction on mongos and on its one participant", function () {
        const prev = pollAllNodes(st, "before single-shard txn");

        const session = st.s.startSession();
        const sessionColl = session.getDatabase(dbName).getCollection(collName);

        session.startTransaction();

        assert.commandWorked(sessionColl.insert({sk: -1, x: 1}));
        assert.commandWorked(sessionColl.update({sk: -1}, {$set: {x: 2}}));
        assert.commandWorked(sessionColl.insert({sk: -2, x: 1}));
        assert.commandWorked(sessionColl.remove({sk: -2}));

        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();

        // No 'secondaries' expectations are given because none are expected to move. assertDeltas()
        // checks that every omitted field stays unchanged, including on the secondaries.
        assertDeltas(st, prev, pollAllNodes(st, "after single-shard txn"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 1,
                commitTypes: {singleShard: 1},
            },
            shards: [
                {
                    primary: {
                        totalStarted: 1,
                        totalStartedExternal: 1,
                        totalCommitted: 1,
                        totalCommittedExternal: 1,
                    },
                },
                {},
            ],
        });
    });

    // A secondary applies a prepared transaction by splitting it across oplog applier writer
    // threads, and each split session is its own TransactionParticipant that bumps these counters,
    // so the exact number depends on how the transaction's operations hash to writer threads.
    // Everything a secondary counts came from an applier thread, which has no client session, so
    // it must be internal.
    const expectedPreparedSecondaryDeltas = {
        totalStarted: atLeast(1),
        totalStartedInternal: atLeast(1),
        totalCommitted: atLeast(1),
        totalCommittedInternal: atLeast(1),
        totalPrepared: atLeast(1),
        totalPreparedInternal: atLeast(1),
        totalPreparedThenCommitted: atLeast(1),
        totalPreparedThenCommittedInternal: atLeast(1),
    };

    it("case 2: classifies a user-initiated multi-shard transaction as an external two-phase commit", function () {
        const prev = pollAllNodes(st, "before multi-shard txn");

        const session = st.s.startSession();
        const sessionColl = session.getDatabase(dbName).getCollection(collName);

        session.startTransaction();

        assert.commandWorked(sessionColl.insert({sk: -10, x: 1}));
        assert.commandWorked(sessionColl.insert({sk: 10, x: 1}));
        assert.commandWorked(sessionColl.update({sk: 10}, {$set: {x: 2}}));
        assert.commandWorked(sessionColl.remove({sk: -10}));

        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();

        const expectedPrimaryDeltas = {
            totalStarted: 1,
            totalStartedExternal: 1,
            totalCommitted: 1,
            totalCommittedExternal: 1,
            totalPrepared: 1,
            totalPreparedExternal: 1,
            totalPreparedThenCommitted: 1,
            totalPreparedThenCommittedExternal: 1,
        };

        assertDeltas(st, prev, pollAllNodes(st, "after multi-shard txn"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitExternal: 1},
            },
            shards: [
                {primary: expectedPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
                {primary: expectedPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
            ],
        });
    });

    // A shard key update that changes a document's shard runs as a two-shard transaction: a delete
    // on the source shard and an insert on the destination shard. mongos starts that transaction
    // itself, so every counter lands on the internal side.
    const expectedWcosPrimaryDeltas = {
        totalStarted: 1,
        totalStartedInternal: 1,
        totalCommitted: 1,
        totalCommittedInternal: 1,
        totalPrepared: 1,
        totalPreparedInternal: 1,
        totalPreparedThenCommitted: 1,
        totalPreparedThenCommittedInternal: 1,
    };

    it("case 3: classifies a retryable-write shard key change as an internal two-phase commit (legacy path)", function () {
        assert.commandWorked(st.s.getDB(dbName)[collName].insert({sk: -100}));

        const prev = pollAllNodes(st, "before legacy WCOS shard key update");

        const session = st.s.startSession({retryWrites: true});
        assert.commandWorked(
            session.getDatabase(dbName)[collName].update({sk: -100}, {$set: {sk: 100}}),
        );
        session.endSession();

        assertDeltas(st, prev, pollAllNodes(st, "after legacy WCOS shard key update"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitInternal: 1},
            },
            shards: [
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
            ],
        });
    });

    it("case 4: classifies a plain shard key change as an internal two-phase commit (transaction API path)", function () {
        assert.commandWorked(st2.s.getDB(dbName)[collName].insert({sk: -100}));

        const prev = pollAllNodes(st2, "before transaction API WCOS shard key update");

        assert.commandWorked(st2.s.getDB(dbName)[collName].update({sk: -100}, {$set: {sk: 100}}));

        assertDeltas(st2, prev, pollAllNodes(st2, "after transaction API WCOS shard key update"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitInternal: 1},
            },
            shards: [
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
            ],
        });
    });
});
