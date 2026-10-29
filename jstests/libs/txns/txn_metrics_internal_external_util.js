/**
 * Helpers for asserting how serverStatus.transactions moves across a sharded cluster, split by
 * whether a transaction is internal (server-initiated) or external (user-initiated).
 */

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
export function atLeast(n) {
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
 * Returns the name and ReplSetTest of every shard in 'shardingTest'.
 */
function getShards(shardingTest) {
    return shardingTest.getAllShards().map((rst, i) => ({
        name: shardingTest["shard" + i].shardName,
        rst,
    }));
}

function getTransactionsSection(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).transactions;
}

/**
 * Reads and logs the transactions section of mongos and of every mongod of every shard. Waits for
 * the secondaries to catch up first, so that their numbers reflect a fully-replicated state
 * rather than a race with oplog application.
 */
export function pollAllNodes(shardingTest, label) {
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
export function assertDeltas(shardingTest, prev, curr, expected) {
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
