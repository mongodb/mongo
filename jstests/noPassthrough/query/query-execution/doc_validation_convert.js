/**
 * A collection validator whose $expr contains a $convert (which must perform a feature flag check)
 * must not crash the server during document validation.
 */
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start");

const testDB = conn.getDB("test");
const coll = testDB[jsTestName()];

assert.commandWorked(
    testDB.createCollection(coll.getName(), {
        validator: {
            $expr: {
                $and: [
                    {
                        $eq: [
                            {
                                $reduce: {
                                    input: "$items",
                                    initialValue: 0,
                                    in: {
                                        $add: ["$$value", {$convert: {input: "$$this", to: "int"}}],
                                    },
                                },
                            },
                            "$total",
                        ],
                    },
                ],
            },
        },
    }),
);

// Before SERVER-128558 this insert crashes the server: validating the document evaluates $convert,
// whose featureFlagMqlJsEngineGap check dereferences the validator's detached (null)
// OperationContext.
assert.commandWorked(coll.insert({_id: 1, items: [1, 2, 3], total: 6}));

// The validator still functions: a document that violates it is rejected rather than crashed on.
assert.commandFailedWithCode(
    coll.insert({_id: 2, items: [1, 2, 3], total: 42}),
    ErrorCodes.DocumentValidationFailure,
);

MongoRunner.stopMongod(conn);
