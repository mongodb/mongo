/**
 * Tests $eq against a variety of BSON types and shapes.
 */

import {show} from "jstests/libs/golden_test.js";
import {leafs, smallDocs} from "jstests/query_golden/libs/example_data.js";

const docs = smallDocs();

const coll = db.query_golden_eq;
coll.drop();

let output = "";

jsTestLog("Inserting docs:");
show(docs);
coll.insert(docs);
print(`Collection count: ${coll.find().itcount()}`);

for (const leaf of leafs()) {
    // Direct comparisons against undefined ({$eq: undefined}) are not allowed.
    if (tojson(leaf).match(/undefined/)) continue;

    const query = coll.find({a: {$eq: leaf}}, {_id: 0});
    jsTestLog(`Query: ${tojsononeline(query._convertToCommand())}`);
    show(query);
}
