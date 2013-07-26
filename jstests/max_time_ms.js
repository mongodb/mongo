// Tests query/command option $maxTimeMS.

var t = db.max_time_ms;
var cursor;

//
// Simple positive test for query: a ~300ms query with a 100ms time limit should be aborted.
//

t.drop();
t.insert([{},{},{}]);
cursor = t.find({$where: function() { sleep(100); return true; }});
cursor.maxTimeMS(100);
assert.throws(function() { cursor.itcount(); }, [], "expected query to abort due to time limit");

//
// Simple negative test for query: a ~300ms query with a 10s time limit should not hit the time
// limit.
//

t.drop();
t.insert([{},{},{}]);
cursor = t.find({$where: function() { sleep(100); return true; }});
cursor.maxTimeMS(10*1000);
assert.doesNotThrow(function() { cursor.itcount(); },
                    [],
                    "expected query to not hit the time limit");

//
// Simple positive test for getmore:
// - Issue a find() that returns 2 batches: a fast batch, then a slow batch.
// - The find() has a 2-second time limit; the first batch should run "instantly", but the second
//   batch takes ~6 seconds, so the getmore should be aborted.
//

t.drop();
t.insert([{},{},{}]); // fast batch
t.insert([{slow: true},{slow: true},{slow: true}]); // slow batch
cursor = t.find({$where: function() {
    if (this.slow) {
        sleep(2*1000);
    }
    return true;
}});
cursor.batchSize(3);
cursor.maxTimeMS(2*1000);
assert.doesNotThrow(function() { cursor.next(); cursor.next(); cursor.next(); },
                    [],
                    "expected batch 1 (query) to not hit the time limit");
assert.throws(function() { cursor.next(); cursor.next(); cursor.next(); },
              [],
              "expected batch 2 (getmore) to abort due to time limit");

//
// Simple negative test for getmore:
// - Issue a find() that returns 2 batches: a fast batch, then a slow batch.
// - The find() has a 10-second time limit; the first batch should run "instantly", and the second
//   batch takes only ~2 seconds, so both the query and getmore should not hit the time limit.
//

t.drop();
t.insert([{},{},{}]); // fast batch
t.insert([{},{},{slow: true}]); // slow batch
cursor = t.find({$where: function() {
    if (this.slow) {
        sleep(2*1000);
    }
    return true;
}});
cursor.batchSize(3);
cursor.maxTimeMS(10*1000);
assert.doesNotThrow(function() { cursor.next(); cursor.next(); cursor.next(); },
                    [],
                    "expected batch 1 (query) to not hit the time limit");
assert.doesNotThrow(function() { cursor.next(); cursor.next(); cursor.next(); },
                    [],
                    "expected batch 2 (getmore) to not hit the time limit");

//
// Many-batch positive test for getmore:
// - Issue a many-batch find() with a 6-second time limit where the results take 10 seconds to
//   generate; one of the later getmore ops should be aborted.
//

t.drop();
for (var i=0; i<5; i++) {
    t.insert([{},{},{slow:true}]);
}
cursor = t.find({$where: function() {
    if (this.slow) {
        sleep(2*1000);
    }
    return true;
}});
cursor.batchSize(3);
cursor.maxTimeMS(6*1000);
assert.throws(function() { cursor.itcount(); }, [], "expected find() to abort due to time limit");

//
// Many-batch negative test for getmore:
// - Issue a many-batch find() with a 20-second time limit where the results take 10 seconds to
//   generate; the find() should not hit the time limit.
//

t.drop();
for (var i=0; i<5; i++) {
    t.insert([{},{},{slow:true}]);
}
cursor = t.find({$where: function() {
    if (this.slow) {
        sleep(2*1000);
    }
    return true;
}});
cursor.batchSize(3);
cursor.maxTimeMS(20*1000);
assert.doesNotThrow(function() { cursor.itcount(); },
                    [],
                    "expected find() to not hit the time limit");

//
// Tests for input validation.
//

t.drop();
t.insert({});

// Verify lower boundary for acceptable input.
assert.doesNotThrow.automsg(function() { t.find().maxTimeMS(0).itcount(); });
assert.doesNotThrow.automsg(function() { t.find().maxTimeMS(NumberInt(0)).itcount(); });
assert.doesNotThrow.automsg(function() { t.find().maxTimeMS(NumberLong(0)).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(-1).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(NumberInt(-1)).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(NumberLong(-1)).itcount(); });

// Verify upper boundary for acceptable input.
var maxValue = Math.pow(2,31)-1;
assert.doesNotThrow.automsg(function() { t.find().maxTimeMS(maxValue).itcount(); });
assert.doesNotThrow.automsg(function() { t.find().maxTimeMS(NumberInt(maxValue)).itcount(); });
assert.doesNotThrow.automsg(function() { t.find().maxTimeMS(NumberLong(maxValue)).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(maxValue+1).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(NumberInt(maxValue+1)).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(NumberLong(maxValue+1)).itcount(); });

// Verify invalid types are rejected.
assert.throws.automsg(function() { t.find().maxTimeMS().itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS("").itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS(true).itcount(); });
assert.throws.automsg(function() { t.find().maxTimeMS({}).itcount(); });
