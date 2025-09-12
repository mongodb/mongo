/**
 * Start two standalones and ensure that hashes match / don't match when
 * the standalones have identical / differing content.
 */

const conn1 = MongoRunner.runMongod();
const conn2 = MongoRunner.runMongod();
const db1 = conn1.getDB("test");
const db2 = conn2.getDB("test");

/**
 * Populates the two standalones with 'contents1' and 'contents2' respectively.
 * Then, depending on the value of 'expectSameHash', asserts that the hash produced
 * by running validate({collHash: true}) either matches or doesn't.
 */
function runCollHashValidateTest(expectSameHash, contents1, contents2) {
    assert(db1.coll.drop());
    assert(db2.coll.drop());
    assert.commandWorked(db1.createCollection("coll"));
    assert.commandWorked(db2.createCollection("coll"));

    jsTest.log.info(`Inserting on db1.coll: ${tojson(contents1)} and on db2.coll: ${tojson(contents2)}`);
    assert.commandWorked(db1.coll.insert(contents1));
    assert.commandWorked(db2.coll.insert(contents2));

    let res1 = assert.commandWorked(db1.coll.validate({collHash: true}));
    let res2 = assert.commandWorked(db2.coll.validate({collHash: true}));

    assert.eq(expectSameHash, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);
}

// Identical contents: empty collections
runCollHashValidateTest(true, [], []);
// Identical contents in the same order.
runCollHashValidateTest(true, [{_id: 1}, {_id: 2}, {_id: 3}], [{_id: 1}, {_id: 2}, {_id: 3}]);
// Identical contents in different orders
runCollHashValidateTest(true, [{_id: 3}, {_id: 1}, {_id: 2}], [{_id: 1}, {_id: 2}, {_id: 3}]);
// Different contents: differing _id value
runCollHashValidateTest(false, [{_id: 4}, {_id: 2}, {_id: 3}], [{_id: 1}, {_id: 2}, {_id: 3}]);
// Differing contents: missing document
runCollHashValidateTest(false, [{_id: 2}, {_id: 3}], [{_id: 1}, {_id: 2}, {_id: 3}]);
// Differing contents: additional field
runCollHashValidateTest(false, [{_id: 1}, {_id: 2}, {_id: 3}], [{_id: 1, a: 1}, {_id: 2}, {_id: 3}]);
// Differing contents: one collection is empty
runCollHashValidateTest(false, [], [{_id: 1}, {_id: 2}, {_id: 3}]);

MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
