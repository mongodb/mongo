// Tests that the basic values returned from the validate command are correct

// Set the number of documents to insert
let count = 10;

function testValidate(output) {
    assert.eq(output.nrecords, count, "validate returned an invalid count");
    assert.eq(output.nIndexes, 3, "validate returned an invalid number of indexes");

    let indexNames = output.keysPerIndex;

    for (let i in indexNames) {
        if (!indexNames.hasOwnProperty(i)) continue;
        assert.eq(indexNames[i], count, "validate returned an invalid number of indexes");
    }
}

// Test to confirm that validate is working as expected.

// SETUP DATA
let t = db.jstests_validate;
t.drop();

for (let i = 0; i < count; i++) {
    t.insert({x: i});
}

t.createIndex({x: 1}, {name: "forward"});
t.createIndex({x: -1}, {name: "reverse"});

// test collHash
var output = t.validate({collHash: true});
testValidate(output);

// Test hashPrefixes
var output = t.validate({collHash: true, hashPrefixes: ["aaa"]});
testValidate(output);

var output = t.validate({collHash: true, hashPrefixes: []});
testValidate(output);

var output = t.validate({hashPrefixes: ["aaa"]});
assert.commandFailed(output);

// Test unhash
var output = t.validate({collHash: true, unhash: ["aaa"]});
testValidate(output);

var output = t.validate({collHash: true, unhash: []});
assert.commandFailed(output);

var output = t.validate({unhash: ["aaa"]});
assert.commandFailed(output);

var output = t.validate({collHash: true, unhash: ["aaa"], hashPrefixes: ["aaa"]});
assert.commandFailed(output);
