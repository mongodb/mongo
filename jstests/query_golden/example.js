/**
 * Example query-correctness test using the golden-data framework.
 */

const coll = db.query_golden_example;
coll.drop();

for (let i = 0; i < 10; ++i) {
    coll.insert({_id: i, a: i});
}

jsTestLog('Collection contents');
show(coll.find());

jsTestLog('ID lookup');
show(coll.find({_id: 5}));
