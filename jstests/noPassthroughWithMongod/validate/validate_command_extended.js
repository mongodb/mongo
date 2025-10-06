// Tests that the basic values returned from the validate command are correct

// Set the number of documents to insert
let count = 10;

function testValidate(options) {
    const output = t.validate(options);
    jsTest.log.info("Testing validate with options: " + tojson(options));
    jsTest.log.info("Validate output: " + tojson(output));

    let indexNames = output.keysPerIndex;

    for (let i in indexNames) {
        if (!indexNames.hasOwnProperty(i)) continue;
        assert.eq(indexNames[i], count, "validate returned an invalid number of indexes");
    }

    if (options.collHash && !options.hashPrefixes) {
        assert(output.all, output);
    }
    if (!options.hashPrefixes) {
        assert.eq(output.nrecords, count, "validate returned an invalid count");
        assert.eq(output.nIndexes, 3, "validate returned an invalid number of indexes");
    } else {
        assert(output.partial, output);
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
testValidate({collHash: true});

// Test hashPrefixes
testValidate({collHash: true, hashPrefixes: ["aaa"]});

testValidate({collHash: true, hashPrefixes: []});

assert.commandFailed(t.validate({hashPrefixes: ["aaa"]}));

assert.commandFailed(t.validate({collHash: true, hashPrefixes: [""]}));

assert.commandFailed(t.validate({collHash: true, hashPrefixes: ["x".repeat(100)]}));

assert.commandFailed(t.validate({collHash: true, hashPrefixes: ["random string"]}));

assert.commandFailed(t.validate({collHash: true, hashPrefixes: ["aaa", "bb"]}));

assert.commandFailed(t.validate({collHash: true, hashPrefixes: ["aaa", "bbb", "aaa"]}));

// Test revealHashedIds
testValidate({collHash: true, revealHashedIds: ["aaa"]});

testValidate({collHash: true, revealHashedIds: ["aaa", "bb"]});

assert.commandFailed(t.validate({collHash: true, revealHashedIds: []}));

assert.commandFailed(t.validate({revealHashedIds: ["aaa"]}));

assert.commandFailed(t.validate({collHash: true, revealHashedIds: ["aaa"], hashPrefixes: ["aaa"]}));

assert.commandFailed(t.validate({collHash: true, revealHashedIds: [""]}));

assert.commandFailed(t.validate({collHash: true, revealHashedIds: ["x".repeat(100)]}));

assert.commandFailed(t.validate({collHash: true, revealHashedIds: ["random string"]}));

assert.commandFailed(t.validate({collHash: true, revealHashedIds: ["aaa", "a", "aaa"]}));
