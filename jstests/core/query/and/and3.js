// Check key match with sub matchers - part of SERVER-3192
// @tags: [
//   assumes_balancer_off,
//   # Uses $where operator
//   requires_scripting,
//   assumes_read_concern_local,
//   requires_fcv_80,
//   requires_getmore,
// ]

let t = db.jstests_and3;
t.drop();

t.save({a: 1});
t.save({a: "foo"});

t.createIndex({a: 1});

function checkScanMatch(query, docsExamined, n) {
    let e = t.find(query).hint({a: 1}).explain("executionStats");
    assert.eq(docsExamined, e.executionStats.totalDocsExamined);
    assert.eq(n, e.executionStats.nReturned);
}

checkScanMatch({a: /o/}, 1, 1);
checkScanMatch({a: /a/}, 0, 0);
checkScanMatch({a: {$not: /o/}}, 2, 1);
checkScanMatch({a: {$not: /a/}}, 2, 2);

checkScanMatch({$and: [{a: /o/}]}, 1, 1);
checkScanMatch({$and: [{a: /a/}]}, 0, 0);
checkScanMatch({$and: [{a: {$not: /o/}}]}, 2, 1);
checkScanMatch({$and: [{a: {$not: /a/}}]}, 2, 2);
checkScanMatch({$and: [{a: /oo/}, {a: {$not: /o/}}]}, 1, 0);
checkScanMatch({$and: [{a: /o/}, {a: {$not: /a/}}]}, 1, 1);
checkScanMatch({$or: [{a: /o/}]}, 1, 1);
checkScanMatch({$or: [{a: /a/}]}, 0, 0);
checkScanMatch({$nor: [{a: /o/}]}, 2, 1);
checkScanMatch({$nor: [{a: /a/}]}, 2, 2);

checkScanMatch({$and: [{$and: [{a: /o/}]}]}, 1, 1);
checkScanMatch({$and: [{$and: [{a: /a/}]}]}, 0, 0);
checkScanMatch({$and: [{$and: [{a: {$not: /o/}}]}]}, 2, 1);
checkScanMatch({$and: [{$and: [{a: {$not: /a/}}]}]}, 2, 2);
checkScanMatch({$and: [{$or: [{a: /o/}]}]}, 1, 1);
checkScanMatch({$and: [{$or: [{a: /a/}]}]}, 0, 0);
checkScanMatch({$or: [{a: {$not: /o/}}]}, 2, 1);
checkScanMatch({$and: [{$or: [{a: {$not: /o/}}]}]}, 2, 1);
checkScanMatch({$and: [{$or: [{a: {$not: /a/}}]}]}, 2, 2);
checkScanMatch({$and: [{$nor: [{a: /o/}]}]}, 2, 1);
checkScanMatch({$and: [{$nor: [{a: /a/}]}]}, 2, 2);

checkScanMatch({$where: "this.a==1"}, 2, 1);
checkScanMatch({$and: [{$where: "this.a==1"}]}, 2, 1);

checkScanMatch({a: 1, $where: "this.a==1"}, 1, 1);
checkScanMatch({a: 1, $and: [{$where: "this.a==1"}]}, 1, 1);
checkScanMatch({$and: [{a: 1}, {$where: "this.a==1"}]}, 1, 1);
checkScanMatch({$and: [{a: 1, $where: "this.a==1"}]}, 1, 1);
checkScanMatch({a: 1, $and: [{a: 1}, {a: 1, $where: "this.a==1"}]}, 1, 1);

assert.eq(0, t.find({a: 1, $and: [{a: 2}]}).itcount());
assert.eq(0, t.find({$and: [{a: 1}, {a: 2}]}).itcount());
