// index skipping and previous index range negation

t = db.jstests_or9;
t.drop();

t.ensureIndex({a: 1, b: 1});

t.save({a: 2, b: 2});

function check(a, q) {
    count = a;
    query = q;
    assert.eq.automsg("count", "t.count( query )");
}

// SERVER-12594: there are two clauses in this case, because we do
// not yet collapse OR of ANDs to a single ixscan.
check(1, {$or: [{a: {$gte: 1, $lte: 3}}, {a: 2}]});

check(1, {$or: [{a: {$gt: 2, $lte: 3}}, {a: 2}]});

check(1, {$or: [{b: {$gte: 1, $lte: 3}}, {b: 2}]});
check(1, {$or: [{b: {$gte: 2, $lte: 3}}, {b: 2}]});
check(1, {$or: [{b: {$gt: 2, $lte: 3}}, {b: 2}]});

// SERVER-12594: there are two clauses in this case, because we do
// not yet collapse OR of ANDs to a single ixscan.
check(1, {$or: [{a: {$gte: 1, $lte: 3}}, {a: 2, b: 2}]});

check(1, {$or: [{a: {$gte: 1, $lte: 3}, b: 3}, {a: 2}]});

check(1, {$or: [{b: {$gte: 1, $lte: 3}}, {b: 2, a: 2}]});

check(1, {$or: [{b: {$gte: 1, $lte: 3}, a: 3}, {b: 2}]});

check(1, {$or: [{a: {$gte: 1, $lte: 3}, b: 3}, {a: 2, b: 2}]});
check(1, {$or: [{a: {$gte: 2, $lte: 3}, b: 3}, {a: 2, b: 2}]});
// SERVER-12594: there are two clauses in this case, because we do
// not yet collapse OR of ANDs to a single ixscan.
check(1, {$or: [{a: {$gte: 1, $lte: 3}, b: 2}, {a: 2, b: 2}]});

check(1, {$or: [{b: {$gte: 1, $lte: 3}, a: 3}, {a: 2, b: 2}]});
check(1, {$or: [{b: {$gte: 2, $lte: 3}, a: 3}, {a: 2, b: 2}]});
// SERVER-12594: there are two clauses in this case, because we do
// not yet collapse OR of ANDs to a single ixscan.
check(1, {$or: [{b: {$gte: 1, $lte: 3}, a: 2}, {a: 2, b: 2}]});

t.remove({});

t.save({a: 1, b: 5});
t.save({a: 5, b: 1});

// SERVER-12594: there are two clauses in the case below, because we do
// not yet collapse OR of ANDs to a single ixscan.
check(2, {$or: [{a: {$in: [1, 5]}, b: {$in: [1, 5]}}, {a: {$in: [1, 5]}, b: {$in: [1, 5]}}]});

check(2, {$or: [{a: {$in: [1]}, b: {$in: [1, 5]}}, {a: {$in: [1, 5]}, b: {$in: [1, 5]}}]});
check(2, {$or: [{a: {$in: [1]}, b: {$in: [1]}}, {a: {$in: [1, 5]}, b: {$in: [1, 5]}}]});
