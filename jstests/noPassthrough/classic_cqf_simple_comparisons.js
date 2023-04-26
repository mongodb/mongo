/**
 * Tests that comparisons against a variety of BSON types and shapes are the same in CQF and
 * classic.
 */
(function() {
"use strict";

load('jstests/query_golden/libs/example_data.js');  // For smallDocs and leafs.

const cqfConn = MongoRunner.runMongod({setParameter: {featureFlagCommonQueryFramework: true}});
assert.neq(null, cqfConn, "mongod was unable to start up");
const cqfDb = cqfConn.getDB(jsTestName());

assert.commandWorked(
    cqfDb.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
const cqfColl = cqfDb.cqf_compare;
cqfColl.drop();

// Disable via TestData so there's no conflict in case a variant has this enabled.
TestData.setParameters.featureFlagCommonQueryFramework = false;
TestData.setParameters.internalQueryFrameworkControl = 'trySbeEngine';
const classicConn = MongoRunner.runMongod();
assert.neq(null, classicConn, "mongod was unable to start up");

const classicColl = classicConn.getDB(jsTestName()).classic_compare;
classicColl.drop();

// TODO SERVER-67818 Bonsai NaN $eq NaN should be true.
// The above ticket also fixes inequality comparisons to NaN.
const docs = smallDocs().filter(doc => !tojson(doc).match(/NaN/));
cqfColl.insert(docs);
classicColl.insert(docs);

for (const op of ['$eq', '$lt', '$lte', '$gt', '$gte']) {
    for (const leaf of leafs()) {
        // TODO SERVER-67550 Equality to null does not match undefined, in Bonsai.
        if (tojson(leaf).match(/null|undefined/))
            continue;
        // TODO SERVER-67818 Bonsai NaN $eq NaN should be true.
        if (tojson(leaf).match(/NaN/))
            continue;
        // Regex with non-equality predicate is not allowed.
        if (leaf instanceof RegExp && op !== '$eq')
            continue;

        const cqfResult = cqfColl.find({a: {[op]: leaf}}, {_id: 0}).toArray();
        const classicResult = classicColl.find({a: {[op]: leaf}}, {_id: 0}).toArray();
        assert.eq(cqfResult, classicResult);
    }
}

MongoRunner.stopMongod(cqfConn);
MongoRunner.stopMongod(classicConn);
}());
