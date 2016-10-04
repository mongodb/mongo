// Test that opcounters get incremented properly.
// Legacy write mode test also available at jstests/gle.

var mongo = new Mongo(db.getMongo().host);

var newdb = mongo.getDB(db.toString());

var t = newdb.opcounters;
var opCounters;
var res;

//
// Count ops attempted in write commands in mongod and mongos
//

//
// 1. Insert.
//
// - unordered insert of N:
//     counted as N ops, regardless of errors
// - ordered insert of N:
//     counted as K + 1 ops, where K is number of docs successfully inserted,
//     adding the failed attempt
//

t.drop();

// Single insert, no error.
opCounters = newdb.serverStatus().opcounters;
res = t.insert({_id: 0});
assert.writeOK(res);
assert.eq(opCounters.insert + 1, newdb.serverStatus().opcounters.insert);

// Bulk insert, no error.
opCounters = newdb.serverStatus().opcounters;
res = t.insert([{_id: 1}, {_id: 2}]);
assert.writeOK(res);
assert.eq(opCounters.insert + 2, newdb.serverStatus().opcounters.insert);

// Test is not run when in compatibility mode as errors are not counted
if (t.getMongo().writeMode() != "compatibility") {
    // Single insert, with error.
    opCounters = newdb.serverStatus().opcounters;
    res = t.insert({_id: 0});
    assert.writeError(res);
    assert.eq(opCounters.insert + 1, newdb.serverStatus().opcounters.insert);

    // Bulk insert, with error, ordered.
    opCounters = newdb.serverStatus().opcounters;
    res = t.insert([{_id: 3}, {_id: 3}, {_id: 4}]);
    assert.writeError(res);
    assert.eq(opCounters.insert + 2, newdb.serverStatus().opcounters.insert);

    // Bulk insert, with error, unordered.
    var continueOnErrorFlag = 1;
    opCounters = newdb.serverStatus().opcounters;
    res = t.insert([{_id: 5}, {_id: 5}, {_id: 6}], continueOnErrorFlag);
    assert.writeError(res);
    assert.eq(opCounters.insert + 3, newdb.serverStatus().opcounters.insert);
}
//
// 2. Update.
//

t.drop();
t.insert({_id: 0});

// Update, no error.
opCounters = newdb.serverStatus().opcounters;
res = t.update({_id: 0}, {$set: {a: 1}});
assert.writeOK(res);
assert.eq(opCounters.update + 1, newdb.serverStatus().opcounters.update);

// Update, with error.
opCounters = newdb.serverStatus().opcounters;
res = t.update({_id: 0}, {$set: {_id: 1}});
assert.writeError(res);
assert.eq(opCounters.update + 1, newdb.serverStatus().opcounters.update);

//
// 3. Delete.
//

t.drop();
t.insert([{_id: 0}, {_id: 1}]);

// Delete, no error.
opCounters = newdb.serverStatus().opcounters;
res = t.remove({_id: 0});
assert.writeOK(res);
assert.eq(opCounters.delete + 1, newdb.serverStatus().opcounters.delete);

// Delete, with error.
opCounters = newdb.serverStatus().opcounters;
res = t.remove({_id: {$invalidOp: 1}});
assert.writeError(res);
assert.eq(opCounters.delete + 1, newdb.serverStatus().opcounters.delete);

//
// 4. Query.
//
// - counted as 1 op, regardless of errors
//

t.drop();
t.insert({_id: 0});

// Query, no error.
opCounters = newdb.serverStatus().opcounters;
t.findOne();
assert.eq(opCounters.query + 1, newdb.serverStatus().opcounters.query);

// Query, with error.
opCounters = newdb.serverStatus().opcounters;
assert.throws(function() {
    t.findOne({_id: {$invalidOp: 1}});
});
assert.eq(opCounters.query + 1, newdb.serverStatus().opcounters.query);

//
// 5. Getmore.
//
// - counted as 1 op per getmore issued, regardless of errors
//

t.drop();
t.insert([{_id: 0}, {_id: 1}, {_id: 2}]);

// Getmore, no error.
opCounters = newdb.serverStatus().opcounters;
t.find().batchSize(2).toArray();  // 3 documents, batchSize=2 => 1 query + 1 getmore
assert.eq(opCounters.query + 1, newdb.serverStatus().opcounters.query);
assert.eq(opCounters.getmore + 1, newdb.serverStatus().opcounters.getmore);

// Getmore, with error (TODO implement when SERVER-5813 is resolved).

//
// 6. Command.
//
// - unrecognized commands not counted
// - recognized commands counted as 1 op, regardless of errors
// - some (recognized) commands can suppress command counting (i.e. aren't counted as commands)
//

t.drop();
t.insert({_id: 0});

// Command, recognized, no error.
serverStatus = newdb.runCommand({serverStatus: 1});
opCounters = serverStatus.opcounters;
metricsObj = serverStatus.metrics.commands;
assert.eq(opCounters.command + 1,
          newdb.serverStatus().opcounters.command);  // "serverStatus" counted
// Count this and the last run of "serverStatus"
assert.eq(metricsObj.serverStatus.total + 2,
          newdb.serverStatus().metrics.commands.serverStatus.total,
          "total ServerStatus command counter did not increment");  // "serverStatus" counted
assert.eq(metricsObj.serverStatus.failed,
          newdb.serverStatus().metrics.commands.serverStatus.failed,
          "failed ServerStatus command counter incremented!");  // "serverStatus" counted

// Command, recognized, with error.
countVal = {
    "total": 0,
    "failed": 0
};
if (metricsObj.count != null) {
    countVal = metricsObj.count;
}
res = t.runCommand("count", {query: {$invalidOp: 1}});  // "count command" counted
assert.eq(0, res.ok);
assert.eq(opCounters.command + 5,
          newdb.serverStatus().opcounters.command);  // "serverStatus", "count" counted

assert.eq(countVal.total + 1,
          newdb.serverStatus().metrics.commands.count.total,
          "total count command counter did not incremented");  // "serverStatus", "count" counted
assert.eq(countVal.failed + 1,
          newdb.serverStatus().metrics.commands.count.failed,
          "failed count command counter did not increment");  // "serverStatus", "count" counted

// Command, unrecognized.
res = t.runCommand("invalid");
assert.eq(0, res.ok);
assert.eq(opCounters.command + 8,
          newdb.serverStatus().opcounters.command);  // "serverStatus" counted
assert.eq(null, newdb.serverStatus().metrics.commands.invalid);
assert.eq(metricsObj['<UNKNOWN>'] + 1, newdb.serverStatus().metrics.commands['<UNKNOWN>']);
