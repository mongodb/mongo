/**
 * Test the $subtype expression.
 * @tags: [
 *  featureFlagMqlJsEngineGap,
 *  requires_fcv_83
 * ]
 */
import {testExpression} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({}));

function runAndAssert(operand, result) {
    // Test with constant-folding optimization.
    testExpression(coll, {$subtype: operand}, result);
    coll.drop();

    // Insert args as document.
    const document = {};
    if (operand !== undefined) {
        document.op = operand;
    }
    assert.commandWorked(coll.insertOne(document));

    // Test again with fields from document.
    assert.eq(coll.aggregate([{$project: {result: {$subtype: "$op"}}}]).toArray()[0].result, result);

    // Clean up.
    coll.drop();
    assert.commandWorked(coll.insertOne({}));
}

function runAndAssertThrows(operand, code) {
    const error = assert.throws(() => coll.aggregate([{$project: {result: {$subtype: operand}}}]).toArray());
    assert.commandFailedWithCode(error, code);
}

// Test BinData subtype.
runAndAssert(BinData(0, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 0);
runAndAssert(BinData(1, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 1);
runAndAssert(BinData(2, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 2);
runAndAssert(BinData(3, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 3);
runAndAssert(BinData(4, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 4);
runAndAssert(BinData(5, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 5);
runAndAssert(BinData(6, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 6);
runAndAssert(BinData(7, "CQDoAwAAAAAAAAA="), 7);
runAndAssert(BinData(8, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 8);
runAndAssert(BinData(9, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 9);
runAndAssert(BinData(128, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 128);

// Test with undocumented subtype.
runAndAssert(BinData(16, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 16);
runAndAssert(BinData(31, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 31);
runAndAssert(BinData(200, "gf1UcxdHTJ2HQ/EGQrO7mQ=="), 200);

// Test with null and missing.
runAndAssert(null, null);
runAndAssert(undefined, null);

// Test with sub-expressions.
testExpression(
    coll,
    {
        $subtype: {
            $convert: {
                input: "867dee52-c331-484e-92d1-c56479b8e67e",
                to: {type: "binData", subtype: 4},
                format: "uuid",
            },
        },
    },
    4,
);
testExpression(coll, {$subtype: UUID("81fd5473-1747-4c9d-8743-f10642b3bb99")}, 4);
testExpression(coll, {$subtype: {$convert: {input: "🙂😎", to: {type: "binData", subtype: 0}, format: "utf8"}}}, 0);

// Test on values without subtype.
const noSubtypeErrorCode = 10389300;

runAndAssertThrows("text", noSubtypeErrorCode);
runAndAssertThrows(10, noSubtypeErrorCode);
runAndAssertThrows([[BinData(0, "gf1UcxdHTJ2HQ/EGQrO7mQ==")]], noSubtypeErrorCode);
runAndAssertThrows({val: BinData(0, "gf1UcxdHTJ2HQ/EGQrO7mQ==")}, noSubtypeErrorCode);
