/**
 * Tests $eq against a variety of BSON types and shapes.
 */

load('jstests/query_golden/libs/example_data.js');

const docs = smallDocs();

const coll = db.query_golden_eq;
coll.drop();

let output = '';

jsTestLog('Inserting docs:');
show(docs);
coll.insert(docs);
print(`Collection count: ${coll.find().itcount()}`);

for (const leaf of leafs()) {
    // TODO SERVER-67550 Equality to null does not match undefined, in Bonsai.
    if (tojson(leaf).match(/null|undefined/))
        continue;
    // TODO SERVER-67818 Bonsai NaN $eq NaN should be true.
    if (tojson(leaf).match(/NaN/))
        continue;

    const query = coll.find({a: {$eq: leaf}}, {_id: 0});
    jsTestLog(`Query: ${tojsononeline(query._convertToCommand())}`);
    show(query);
}
