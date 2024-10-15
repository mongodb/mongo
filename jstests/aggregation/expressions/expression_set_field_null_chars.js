/**
 * Tests that $setField handles null chars in the 'field' argument correctly.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   featureFlagBulkWriteCommand,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 1, foo: "bar"}));

// Asserts $setField and $unsetField with the given 'field' argument fail with one of the given
// 'codes' when used in various commands.
function assertSetFieldFailsWithCode({field, codes}) {
    const setFieldExpressions = [
        {$setField: {field, input: {}, value: true}},
        {$unsetField: {field, input: {}}},
    ];

    for (const setFieldExpression of setFieldExpressions) {
        const errorMsg = tojson(setFieldExpression);

        assert.commandFailedWithCode(
            db.runCommand({find: coll.getName(), projection: {field: setFieldExpression}}),
            codes,
            errorMsg,
        );
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$project: {field: setFieldExpression}}, {$out: coll.getName() + "-2"}],
                cursor: {}
            }),
            codes,
            errorMsg,
        );
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{
                    $merge: {
                        into: coll.getName(),
                        whenMatched: [{$replaceWith: setFieldExpression}],
                        whenNotMatched: "discard"
                    }
                }],
                cursor: {}
            }),
            codes,
            errorMsg,
        );
        assert.commandFailedWithCode(
            db.runCommand({
                update: coll.getName(),
                updates: [{q: {_id: 1}, u: [{$replaceWith: setFieldExpression}], multi: false}]
            }),
            codes,
            errorMsg,
        );
        assert.commandFailedWithCode(
            db.runCommand({
                findAndModify: coll.getName(),
                query: {_id: 1},
                update: [{$replaceWith: setFieldExpression}]
            }),
            codes,
            errorMsg,
        );
        // Only the nested update command fails here. The shell helper 'coll.bulkWrite()' would
        // throw in that case, but 'assert.throwsWithCode()' can't pull the error code from
        // 'BulkWriteError'.
        const bulkWriteRes = assert.commandWorked(db.adminCommand({
            bulkWrite: 1,
            ops: [{update: 0, filter: {_id: 1}, updateMods: [{$replaceWith: setFieldExpression}]}],
            nsInfo: [{ns: `${db.getName()}.${coll.getName()}`}],
        }));
        assert(codes.some(code => code == bulkWriteRes.cursor.firstBatch[0].code));
    }
}

const invalidFieldNames = [
    // Starts with null chars.
    "\x00a",
    // Ends with null chars.
    "a\x00",
    // All null chars.
    "\x00",
    "\x00\x00\x00",
    // Null chars somewhere in the middle.
    "a\x00\x01\x08a",
    "a\x00\x02\x08b",
    "a\x00\x01\x18b",
    "a\x00\x01\x28c",
    "a\x00\x01\x03d\x00\xff\xff\xff\xff\x00\x08b",
];

// Test each field name directly, as part of a field reference, wrapped in $const/$literal,
// wrapped in a const-foldable expression as well as some combinations of these.
for (const field of invalidFieldNames) {
    assertSetFieldFailsWithCode({field, codes: [9534700, 9423101]});
    assertSetFieldFailsWithCode({field: {$const: field}, codes: [9534700, 9423101]});
    assertSetFieldFailsWithCode({field: {$literal: field}, codes: [9534700, 9423101]});
    assertSetFieldFailsWithCode({field: "$" + field, codes: [16411, 9423101]});
    assertSetFieldFailsWithCode({field: "$foo." + field, codes: [16411, 9423101]});
    assertSetFieldFailsWithCode({field: "foo." + field, codes: [9534700, 9423101]});
    assertSetFieldFailsWithCode({field: {$const: "$" + field}, codes: [9534700, 9423101]});
    assertSetFieldFailsWithCode({field: {$const: "$foo." + field}, codes: [9534700, 9423101]});
    // Sanity check: non-literal expressions are not allowed even when they could be const
    // folded.
    assertSetFieldFailsWithCode({field: {$concat: [field]}, codes: [4161106, 9423101]});
    assertSetFieldFailsWithCode({field: {$toUpper: field}, codes: [4161106, 9423101]});
    assertSetFieldFailsWithCode({field: {$toLower: field}, codes: [4161106, 9423101]});
}
