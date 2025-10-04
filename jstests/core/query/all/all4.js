// Test $all/$elemMatch with missing field - SERVER-4492
// @tags: [
//   requires_getmore
// ]

let t = db.jstests_all4;
t.drop();

function checkQuery(query, val) {
    assert.eq(val, t.count(query));
    assert.eq(val, t.find(query).itcount());
}

checkQuery({a: {$all: []}}, 0);
checkQuery({a: {$all: [1]}}, 0);
checkQuery({a: {$all: [{$elemMatch: {b: 1}}]}}, 0);

t.save({});
checkQuery({a: {$all: []}}, 0);
checkQuery({a: {$all: [1]}}, 0);
checkQuery({a: {$all: [{$elemMatch: {b: 1}}]}}, 0);

t.save({a: 1});
checkQuery({a: {$all: []}}, 0);
checkQuery({a: {$all: [1]}}, 1);
checkQuery({a: {$all: [{$elemMatch: {b: 1}}]}}, 0);

t.save({a: [{b: 1}]});
checkQuery({a: {$all: []}}, 0);
checkQuery({a: {$all: [1]}}, 1);
checkQuery({a: {$all: [{$elemMatch: {b: 1}}]}}, 1);
