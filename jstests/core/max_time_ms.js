// Tests query/command option $maxTimeMS.
//
// @tags: [
//   # This test attempts to perform read operations after having enabled the maxTimeAlwaysTimeOut
//   # failpoint. The former operations may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_preference_unchanged,
//   requires_fastcount,
//   requires_getmore,
//   # Uses $where operator
//   requires_scripting,
//   uses_testing_only_commands,
// ]

var t = db.max_time_ms;
var cursor;
var res;
var error;

//
// Simple positive test for query: a ~100 second query with a 100ms time limit should be aborted.
//

t.drop();
assert.commandWorked(t.insert(Array.from({length: 1000}, _ => ({}))));

cursor = t.find({
    $where: function() {
        sleep(100);
        return true;
    }
});
cursor.maxTimeMS(100);
error = assert.throws(function() {
    cursor.itcount();
}, [], "expected query to abort due to time limit");
// TODO SERVER-32565: The error should always be MaxTimeMSExpired, but there are rare cases where
// interrupting javascript execution on the server with a stepdown or timeout results in an error
// code of InternalError or Interrupted instead, so we also accept those here.
assert.contains(error.code,
                [ErrorCodes.MaxTimeMSExpired, ErrorCodes.Interrupted, ErrorCodes.InternalError],
                "Failed with error: " + tojson(error));

//
// Simple negative test for query: a ~300ms query with a 10s time limit should not hit the time
// limit.
//

t.drop();
assert.commandWorked(t.insert([{}, {}, {}]));
cursor = t.find({
    $where: function() {
        sleep(100);
        return true;
    }
});
cursor.maxTimeMS(10 * 1000);
assert.doesNotThrow(function() {
    cursor.itcount();
}, [], "expected query to not hit the time limit");

//
// Simple positive test for getmore:
// - Issue a find() that returns 2 batches: a fast batch, then a slow batch.
// - The find() has a 4-second time limit; the first batch should run "instantly", but the second
//   batch takes ~15 seconds, so the getmore should be aborted.
//

t.drop();
assert.commandWorked(t.insert([{_id: 0}, {_id: 1}, {_id: 2}]));  // fast batch
assert.commandWorked(
    t.insert([{_id: 3, slow: true}, {_id: 4, slow: true}, {_id: 5, slow: true}]));  // slow batch
cursor = t.find({
              $where: function() {
                  if (this.slow) {
                      sleep(5 * 1000);
                  }
                  return true;
              }
          }).sort({_id: 1});
cursor.batchSize(3);
cursor.maxTimeMS(4 * 1000);
assert.doesNotThrow(function() {
    cursor.next();
    cursor.next();
    cursor.next();
}, [], "expected batch 1 (query) to not hit the time limit");
error = assert.throws(function() {
    cursor.next();
    cursor.next();
    cursor.next();
}, [], "expected batch 2 (getmore) to abort due to time limit");
// TODO SERVER-32565: The error should always be MaxTimeMSExpired, but there are rare cases where
// interrupting javascript execution on the server with a stepdown or timeout results in an error
// code of InternalError or Interrupted instead, so we also accept those here.
assert.contains(error.code,
                [ErrorCodes.MaxTimeMSExpired, ErrorCodes.Interrupted, ErrorCodes.InternalError],
                "Failed with error: " + tojson(error));

//
// Simple negative test for getmore:
// - Issue a find() that returns 2 batches: a fast batch, then a slow batch.
// - The find() has a 10-second time limit; the first batch should run "instantly", and the second
//   batch takes only ~2 seconds, so both the query and getmore should not hit the time limit.
//

t.drop();
assert.commandWorked(t.insert([{_id: 0}, {_id: 1}, {_id: 2}]));              // fast batch
assert.commandWorked(t.insert([{_id: 3}, {_id: 4}, {_id: 5, slow: true}]));  // slow batch
cursor = t.find({
              $where: function() {
                  if (this.slow) {
                      sleep(2 * 1000);
                  }
                  return true;
              }
          }).sort({_id: 1});
cursor.batchSize(3);
cursor.maxTimeMS(10 * 1000);
assert.doesNotThrow(function() {
    cursor.next();
    cursor.next();
    cursor.next();
}, [], "expected batch 1 (query) to not hit the time limit");
assert.doesNotThrow(function() {
    cursor.next();
    cursor.next();
    cursor.next();
}, [], "expected batch 2 (getmore) to not hit the time limit");

//
// Many-batch positive test for getmore:
// - Issue a many-batch find() with a 6-second time limit where the results take 10 seconds to
//   generate; one of the later getmore ops should be aborted.
//

t.drop();
for (var i = 0; i < 5; i++) {
    assert.commandWorked(
        t.insert([{_id: 3 * i}, {_id: (3 * i) + 1}, {_id: (3 * i) + 2, slow: true}]));
}
cursor = t.find({
              $where: function() {
                  if (this.slow) {
                      sleep(2 * 1000);
                  }
                  return true;
              }
          }).sort({_id: 1});
cursor.batchSize(3);
cursor.maxTimeMS(6 * 1000);
error = assert.throws(function() {
    cursor.itcount();
}, [], "expected find() to abort due to time limit");
// TODO SERVER-32565: The error should always be MaxTimeMSExpired, but there are rare cases where
// interrupting javascript execution on the server with a stepdown or timeout results in an error
// code of InternalError or Interrupted instead, so we also accept those here.
assert.contains(error.code,
                [ErrorCodes.MaxTimeMSExpired, ErrorCodes.Interrupted, ErrorCodes.InternalError],
                "Failed with error: " + tojson(error));

//
// Many-batch negative test for getmore:
// - Issue a many-batch find() with a 20-second time limit where the results take 10 seconds to
//   generate; the find() should not hit the time limit.
//

t.drop();
for (var i = 0; i < 5; i++) {
    assert.commandWorked(
        t.insert([{_id: 3 * i}, {_id: (3 * i) + 1}, {_id: (3 * i) + 2, slow: true}]));
}
cursor = t.find({
              $where: function() {
                  if (this.slow) {
                      sleep(2 * 1000);
                  }
                  return true;
              }
          }).sort({_id: 1});
cursor.batchSize(3);
cursor.maxTimeMS(20 * 1000);
assert.doesNotThrow(function() {
    // SERVER-40305: Add some additional logging here in case this fails to help us track down why
    // it failed.
    assert.commandWorked(db.adminCommand({setParameter: 1, traceExceptions: 1}));
    cursor.itcount();
    assert.commandWorked(db.adminCommand({setParameter: 1, traceExceptions: 0}));
}, [], "expected find() to not hit the time limit");

//
// Simple positive test for commands: a ~300ms command with a 100ms time limit should be aborted.
//

t.drop();
res = t.getDB().adminCommand({sleep: 1, millis: 300, maxTimeMS: 100});
assert(res.ok == 0 && res.code == ErrorCodes.MaxTimeMSExpired,
       "expected sleep command to abort due to time limit, ok=" + res.ok + ", code=" + res.code);

//
// Simple negative test for commands: a ~300ms command with a 10s time limit should not hit the
// time limit.
//

t.drop();
res = t.getDB().adminCommand({sleep: 1, millis: 300, maxTimeMS: 10 * 1000});
assert(res.ok == 1,
       "expected sleep command to not hit the time limit, ok=" + res.ok + ", code=" + res.code);

//
// Tests for input validation.
//

t.drop();
assert.commandWorked(t.insert({}));

// Verify lower boundary for acceptable input (0 is acceptable, 1 isn't).

assert.doesNotThrow.automsg(function() {
    t.find().maxTimeMS(0).itcount();
});
assert.doesNotThrow.automsg(function() {
    t.find().maxTimeMS(NumberInt(0)).itcount();
});
assert.doesNotThrow.automsg(function() {
    t.find().maxTimeMS(NumberLong(0)).itcount();
});
assert.eq(1, t.getDB().runCommand({ping: 1, maxTimeMS: 0}).ok);
assert.eq(1, t.getDB().runCommand({ping: 1, maxTimeMS: NumberInt(0)}).ok);
assert.eq(1, t.getDB().runCommand({ping: 1, maxTimeMS: NumberLong(0)}).ok);

assert.throws.automsg(function() {
                 t.find().maxTimeMS(-1).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS(NumberInt(-1)).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS(NumberLong(-1)).itcount();
             });
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: -1}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: NumberInt(-1)}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: NumberLong(-1)}).ok);

// Verify upper boundary for acceptable input (2^31-1 is acceptable, 2^31 isn't).

var maxValue = Math.pow(2, 31) - 1;

assert.doesNotThrow.automsg(function() {
    t.find().maxTimeMS(maxValue).itcount();
});
assert.doesNotThrow.automsg(function() {
    t.find().maxTimeMS(NumberInt(maxValue)).itcount();
});
assert.doesNotThrow.automsg(function() {
    t.find().maxTimeMS(NumberLong(maxValue)).itcount();
});
assert.eq(1, t.getDB().runCommand({ping: 1, maxTimeMS: maxValue}).ok);
assert.eq(1, t.getDB().runCommand({ping: 1, maxTimeMS: NumberInt(maxValue)}).ok);
assert.eq(1, t.getDB().runCommand({ping: 1, maxTimeMS: NumberLong(maxValue)}).ok);

assert.throws.automsg(function() {
                 t.find().maxTimeMS(maxValue + 1).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS(NumberInt(maxValue + 1)).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS(NumberLong(maxValue + 1)).itcount();
             });
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: maxValue + 1}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: NumberInt(maxValue + 1)}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: NumberLong(maxValue + 1)}).ok);

// Verify invalid values are rejected.
assert.throws.automsg(function() {
                 t.find().maxTimeMS(0.1).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS(-0.1).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS().itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS("").itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS(true).itcount();
             });
assert.throws.automsg(function() {
                 t.find().maxTimeMS({}).itcount();
             });
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: 0.1}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: -0.1}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: undefined}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: ""}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: true}).ok);
assert.eq(0, t.getDB().runCommand({ping: 1, maxTimeMS: {}}).ok);

// Verify that the maxTimeMS command argument can be sent with $query-wrapped commands.
cursor = t.getDB().$cmd.find({ping: 1, maxTimeMS: 0}).limit(-1);
cursor._ensureSpecial();
assert.eq(1, cursor.next().ok);

// Verify that the server rejects invalid command argument $maxTimeMS.
cursor = t.getDB().$cmd.find({ping: 1, $maxTimeMS: 0}).limit(-1);
cursor._ensureSpecial();
assert.eq(0, cursor.next().ok);

// Verify that the $maxTimeMS query option can't be sent with $query-wrapped commands.
cursor = t.getDB().$cmd.find({ping: 1}).limit(-1).maxTimeMS(0);
cursor._ensureSpecial();
assert.commandFailed(cursor.next());

//
// Tests for fail points maxTimeAlwaysTimeOut and maxTimeNeverTimeOut.
//

// maxTimeAlwaysTimeOut positive test for command.
t.drop();
assert.eq(
    1, t.getDB().adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}).ok);
res = t.getDB().runCommand({ping: 1, maxTimeMS: 10 * 1000});
assert(res.ok == 0 && res.code == ErrorCodes.MaxTimeMSExpired,
       "expected command to trigger maxTimeAlwaysTimeOut fail point, ok=" + res.ok +
           ", code=" + res.code);
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}).ok);

// maxTimeNeverTimeOut positive test for command.
t.drop();
assert.eq(1,
          t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}).ok);
res = t.getDB().adminCommand({sleep: 1, millis: 300, maxTimeMS: 100});
assert(res.ok == 1,
       "expected command to trigger maxTimeNeverTimeOut fail point, ok=" + res.ok +
           ", code=" + res.code);
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}).ok);

// maxTimeAlwaysTimeOut positive test for query.
t.drop();
assert.eq(
    1, t.getDB().adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}).ok);
assert.throws(function() {
    t.find().maxTimeMS(10 * 1000).itcount();
}, [], "expected query to trigger maxTimeAlwaysTimeOut fail point");
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}).ok);

// maxTimeNeverTimeOut positive test for query.
assert.eq(1,
          t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}).ok);
t.drop();
assert.commandWorked(t.insert([{}, {}, {}]));
cursor = t.find({
    $where: function() {
        sleep(100);
        return true;
    }
});
cursor.maxTimeMS(100);
assert.doesNotThrow(function() {
    cursor.itcount();
}, [], "expected query to trigger maxTimeNeverTimeOut fail point");
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}).ok);

// maxTimeAlwaysTimeOut positive test for getmore.
t.drop();
assert.commandWorked(t.insert([{}, {}, {}]));
cursor = t.find().maxTimeMS(10 * 1000).batchSize(2);
assert.doesNotThrow.automsg(function() {
    cursor.next();
    cursor.next();
});
assert.eq(
    1, t.getDB().adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}).ok);
assert.throws(function() {
    cursor.next();
}, [], "expected getmore to trigger maxTimeAlwaysTimeOut fail point");
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}).ok);

// maxTimeNeverTimeOut positive test for getmore.
t.drop();
assert.commandWorked(t.insert([{_id: 0}, {_id: 1}, {_id: 2}]));  // fast batch
assert.commandWorked(
    t.insert([{_id: 3, slow: true}, {_id: 4, slow: true}, {_id: 5, slow: true}]));  // slow batch
cursor = t.find({
              $where: function() {
                  if (this.slow) {
                      sleep(2 * 1000);
                  }
                  return true;
              }
          }).sort({_id: 1});
cursor.batchSize(3);
cursor.maxTimeMS(2 * 1000);
assert.doesNotThrow(function() {
    cursor.next();
    cursor.next();
    cursor.next();
}, [], "expected batch 1 (query) to not hit the time limit");
assert.eq(1,
          t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}).ok);
assert.doesNotThrow(function() {
    cursor.next();
    cursor.next();
    cursor.next();
}, [], "expected batch 2 (getmore) to trigger maxTimeNeverTimeOut fail point");
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}).ok);

//
// Test that maxTimeMS is accepted by commands that have an option whitelist.
//

// "aggregate" command.
res = t.runCommand("aggregate", {pipeline: [], cursor: {}, maxTimeMS: 60 * 1000});
assert(res.ok == 1,
       "expected aggregate with maxtime to succeed, ok=" + res.ok + ", code=" + res.code);

// "collMod" command.
res = t.runCommand("collMod", {maxTimeMS: 60 * 1000});
assert(res.ok == 1,
       "expected collmod with maxtime to succeed, ok=" + res.ok + ", code=" + res.code);

// "createIndexes" command.
assert.commandWorked(
    t.runCommand("createIndexes", {indexes: [{key: {x: 1}, name: "x_1"}], maxTimeMS: 60 * 1000}));

//
// test count shell helper SERVER-13334
//
t.drop();
assert.eq(1,
          t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}).ok);
assert.doesNotThrow(function() {
    t.find({}).maxTimeMS(1).count();
});
assert.eq(1, t.getDB().adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}).ok);
