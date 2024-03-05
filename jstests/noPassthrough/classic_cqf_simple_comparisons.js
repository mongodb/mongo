/**
 * Tests that comparisons against a variety of BSON types and shapes are the same in CQF and
 * classic.
 */
import {leafs, smallDocs} from "jstests/query_golden/libs/example_data.js";

const cqfConn = MongoRunner.runMongod({setParameter: {featureFlagCommonQueryFramework: true}});
assert.neq(null, cqfConn, "mongod was unable to start up");
const cqfDb = cqfConn.getDB(jsTestName());

assert.commandWorked(
    cqfDb.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
const cqfColl = cqfDb.cqf_compare;
cqfColl.drop();

const classicConn = MongoRunner.runMongod({
    setParameter:
        {featureFlagCommonQueryFramework: false, internalQueryFrameworkControl: "trySbeEngine"}
});
assert.neq(null, classicConn, "mongod was unable to start up");

const classicColl = classicConn.getDB(jsTestName()).classic_compare;
classicColl.drop();

// The above ticket also fixes inequality comparisons to NaN.
const docs = smallDocs().filter(doc => !tojson(doc).match(/NaN/));
cqfColl.insert(docs);
classicColl.insert(docs);

for (const op of ['$eq', '$lt', '$lte', '$gt', '$gte']) {
    for (const leaf of leafs()) {
        // Direct comparisons against undefined ({$eq: undefined}) are not allowed.
        if (tojson(leaf).match(/undefined/))
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
