/*
 * Tests for the $isNumber aggregation expression.
 */
(function() {
'use strict';
const coll = db.isNumber_expr;
coll.drop();

function testIsNumber(inputExprPath, expectedOutput, inputId) {
    const result = coll.aggregate([
                           {"$match": {_id: inputId}},
                           {"$project": {_id: 0, "isNum": {"$isNumber": inputExprPath}}},
                       ])
                       .toArray();
    assert.eq(result, expectedOutput);
}

// Test when $isNumber evaluates to an integer, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 1, integerFieldPath: NumberInt(56072)}));
testIsNumber("$integerFieldPath", [{"isNum": true}], 1);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 11, nestedPath: {nestedNumber: NumberInt(500)}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": true}], 11);

// Test when $isNumber evaluates to a double, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 2, doubleFieldPath: 56072.2}));
testIsNumber("$doubleFieldPath", [{"isNum": true}], 2);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 12, nestedPath: {nestedNumber: 56072.2}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": true}], 12);

// Test when $isNumber evaluates to a decimal, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 3, decimalFieldPath: NumberDecimal("1.234")}));
testIsNumber("$decimalFieldPath", [{"isNum": true}], 3);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 13, nestedPath: {nestedNumber: NumberDecimal("1.234")}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": true}], 13);

// Test when $isNumber evaluates to a long when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 4, longFieldPath: NumberLong("123456789")}));
testIsNumber("$longFieldPath", [{"isNum": true}], 4);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 14, nestedPath: {nestedNumber: NumberLong("123456789")}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": true}], 14);

// Test when $isNumber evaluates to null, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 5, nullFieldPath: null}));
testIsNumber("$nullFieldPath", [{"isNum": false}], 5);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 15, nestedPath: {nestedNull: null}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": false}], 15);

// Test when $isNumber evaluates to missing, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 6}));
testIsNumber("$missingFieldPath", [{"isNum": false}], 6);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 16, nestedPath: {}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": false}], 16);

// Test when $isNumber evaluates to an array, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 7, arrayFieldPath: [1]}));
testIsNumber("$arrayFieldPath", [{"isNum": false}], 7);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 17, nestedPath: {nestedArray: [1]}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": false}], 17);

// Test when $isNumber evaluates to a string, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 8, stringFieldPath: "1234"}));
testIsNumber("$stringFieldPath", [{"isNum": false}], 8);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 18, nestedPath: {nestedString: "12345"}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": false}], 18);

// Test when $isNumber evaluates to a Date, when the input expression is a document field.
assert.commandWorked(coll.insert({_id: 9, dateFieldPath: new Date()}));
testIsNumber("$dateFieldPath", [{"isNum": false}], 9);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 19, nestedPath: {nestedDate: new Date()}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": false}], 19);

// Test when $isNumber evaluates to a BinData, when the input expression is a document field.
// (UUID's are encoded as binary data)
assert.commandWorked(coll.insert({_id: 10, binDataFieldPath: UUID()}));
testIsNumber("$binDataFieldPath", [{"isNum": false}], 10);
// Same as above, but when the input expression is a nested document field.
assert.commandWorked(coll.insert({_id: 20, nestedPath: {nestedBinData: UUID()}}));
testIsNumber("$nestedPath.nestedNumber", [{"isNum": false}], 20);

// Test a few literal expressions, rather than ones retrieved from a document.
assert.commandWorked(coll.insert({_id: 21}));

// Test when $isNumber's input expression is a literal long.
testIsNumber(NumberLong("12345678"), [{"isNum": true}], 21);

// Test when $isNumber's input expression is a literal decimal.
testIsNumber(NumberDecimal("1.2345678"), [{"isNum": true}], 21);

// Test when $isNumber's input expression is a literal int.
testIsNumber(NumberInt(18), [{"isNum": true}], 21);

// Test when $isNumber's input expression is a literal double.
testIsNumber(5.34, [{"isNum": true}], 21);

// Test when $isNumber's input expression is null literal.
testIsNumber(null, [{"isNum": false}], 21);

// Test when $isNumber's input expression is a literal array.
testIsNumber([[2]], [{"isNum": false}], 21);

// Test when $isNumber's input expression is a literal string.
testIsNumber("a literal string", [{"isNum": false}], 21);

// Test when $isNumber's input expression is a literal Date.
testIsNumber(new Date(), [{"isNum": false}], 21);

// Test when $isNumber's input expression is a literal BinData.
testIsNumber(UUID(), [{"isNum": false}], 21);
}());
