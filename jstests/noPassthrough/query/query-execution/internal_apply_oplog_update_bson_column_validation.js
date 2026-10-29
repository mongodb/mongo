/*
 * Tests BSONColumns in _internalApplyOplogUpdate are validated.
 */
const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

// String literal "foo" + EOO -- a valid BSONColumn.
const validColumn = BinData(7, "AgAEAAAAZm9vAAA=");
// String literal "foo" without EOO -- an invalid BSONColumn.
const invalidColumnPayload = BinData(0, "AgAEAAAAZm9vAA==");

function runCase(id, doc, diff) {
    assert.commandWorked(coll.insert(Object.assign({_id: id}, doc)));
    assert.commandFailedWithCode(
        coll.runCommand({
            update: coll.getName(),
            updates: [
                {
                    q: {_id: id},
                    u: [
                        {
                            $_internalApplyOplogUpdate: {
                                oplogUpdate: {$v: NumberInt(2), diff: diff},
                            },
                        },
                    ],
                },
            ],
        }),
        ErrorCodes.InvalidBSONColumn,
    );
}

// Case A: BSONColumn one level inside an object.
runCase(
    0,
    {control: {version: NumberInt(2)}, data: {t: validColumn}},
    {sdata: {b: {t: {o: NumberInt(0), d: invalidColumnPayload}}}},
);

// Case B: BSONColumn deeply nested. Exercises the recursive `deep` path in
// storage_validation::scanDocument.
runCase(
    1,
    {control: {version: NumberInt(2)}, outer: {inner: {data: {t: validColumn}}}},
    {souter: {sinner: {sdata: {b: {t: {o: NumberInt(0), d: invalidColumnPayload}}}}}},
);

MongoRunner.stopMongod(conn);
