/**
 * Tests that write commands validate stmtIds values.
 *
 * @tags: [
 *   # bulkWrite blocked under a security token.
 *   not_allowed_with_signed_security_token,
 *   # bulkWrite unsupported in serverless.
 *   command_not_supported_in_serverless,
 *   # Retryable writes injects colliding stmtIds.
 *   requires_non_retryable_writes,
 *   does_not_support_retryable_writes,
 *   # Transactions inject colliding stmtIds.
 *   does_not_support_transactions,
 *   # Implicit sharding makes the timeseries a regular collection.
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   # bulkWrite introduced in 8.0.
 *   requires_fcv_80,
 * ]
 */
const adminDB = db.getSiblingDB("admin");

const regularColl = jsTestName() + "_regular";
const timeseriesColl = jsTestName() + "_timeseries";
db[regularColl].drop();
db[timeseriesColl].drop();
db.createCollection(regularColl);
db.createCollection(timeseriesColl, {timeseries: {timeField: "t", metaField: "m"}});

function measurement(metaValue) {
    return {t: new Date(), m: metaValue, v: 1};
}

function twoOpCommands(coll, ns) {
    return [
        {insert: coll, documents: [measurement(0), measurement(1)]},
        {
            update: coll,
            updates: [
                {q: {m: 0}, u: {$set: {v: 2}}},
                {q: {m: 1}, u: {$set: {v: 2}}},
            ],
        },
        {
            delete: coll,
            deletes: [
                {q: {m: 0}, limit: 1},
                {q: {m: 1}, limit: 1},
            ],
        },
        {
            db: adminDB,
            bulkWrite: 1,
            ops: [
                {insert: 0, document: measurement(0)},
                {insert: 0, document: measurement(1)},
            ],
            nsInfo: [{ns: ns}],
        },
    ];
}

function runWithStmtIds(cmd, stmtIds) {
    const runDB = cmd.db || db;
    const {db: _, ...rest} = cmd;
    return runDB.runCommand(Object.assign(rest, {stmtIds: stmtIds}));
}

// A single placeholder is fine.
assert.commandWorked(
    db.runCommand({insert: regularColl, documents: [measurement(2)], stmtIds: [NumberInt(-1)]}),
);

// Every operation accepts valid stmtIds, and placeholders may repeat, mix with assigned ids, and
// appear in any position.
const accepted = [
    [NumberInt(1), NumberInt(2)],
    [NumberInt(-1), NumberInt(-1)],
    [NumberInt(-1), NumberInt(2)],
    [NumberInt(1), NumberInt(-1)],
];
const regularNs = db.getName() + "." + regularColl;
for (const cmd of twoOpCommands(regularColl, regularNs)) {
    for (const stmtIds of accepted) {
        assert.commandWorked(runWithStmtIds(cmd, stmtIds));
    }
}

// Duplicate assigned ids and non-placeholder negatives must be rejected on every operation.
const rejected = [
    [NumberInt(0), NumberInt(0)],
    [NumberInt(-2), NumberInt(1)],
];

for (const coll of [regularColl, timeseriesColl]) {
    const ns = db.getName() + "." + coll;
    for (const cmd of twoOpCommands(coll, ns)) {
        for (const stmtIds of rejected) {
            assert.commandFailedWithCode(runWithStmtIds(cmd, stmtIds), ErrorCodes.InvalidOptions);
        }
    }
}
