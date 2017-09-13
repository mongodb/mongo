// Test that opcounters get incremented properly.
// Write command version also available at jstests/core.

// Remember the global 'db' var
var lastDB = db;
var mongo = new Mongo(db.getMongo().host);
mongo.writeMode = function() {
    return "legacy";
};
db = mongo.getDB(db.toString());

var t = db.opcounters;
var isMongos = ("isdbgrid" == db.runCommand("ismaster").msg);
var opCounters;

//
// 1. Insert.
//
// - mongod, single insert:
//     counted as 1 op if successful, else 0
// - mongod, bulk insert of N with continueOnError=true:
//     counted as K ops, where K is number of docs successfully inserted
// - mongod, bulk insert of N with continueOnError=false:
//     counted as K ops, where K is number of docs successfully inserted
//
// - mongos
//     count ops attempted like insert commands
//

t.drop();

// Single insert, no error.
opCounters = db.serverStatus().opcounters;
t.insert({_id: 0});
assert(!db.getLastError());
assert.eq(opCounters.insert + 1, db.serverStatus().opcounters.insert);

// Bulk insert, no error.
opCounters = db.serverStatus().opcounters;
t.insert([{_id: 1}, {_id: 2}]);
assert(!db.getLastError());
assert.eq(opCounters.insert + 2, db.serverStatus().opcounters.insert);

// Single insert, with error.
opCounters = db.serverStatus().opcounters;
t.insert({_id: 0});
print(db.getLastError());
assert(db.getLastError());
assert.eq(opCounters.insert + 1, db.serverStatus().opcounters.insert);

// Bulk insert, with error, continueOnError=false.
opCounters = db.serverStatus().opcounters;
t.insert([{_id: 3}, {_id: 3}, {_id: 4}]);
assert(db.getLastError());
assert.eq(opCounters.insert + 2, db.serverStatus().opcounters.insert);

// Bulk insert, with error, continueOnError=true.
var continueOnErrorFlag = 1;
opCounters = db.serverStatus().opcounters;
t.insert([{_id: 5}, {_id: 5}, {_id: 6}], continueOnErrorFlag);
assert(db.getLastError());
assert.eq(opCounters.insert + (isMongos ? 2 : 3), db.serverStatus().opcounters.insert);

//
// 2. Update.
//
// - counted as 1 op, regardless of errors
//

t.drop();
t.insert({_id: 0});

// Update, no error.
opCounters = db.serverStatus().opcounters;
t.update({_id: 0}, {$set: {a: 1}});
assert(!db.getLastError());
assert.eq(opCounters.update + 1, db.serverStatus().opcounters.update);

// Update, with error.
opCounters = db.serverStatus().opcounters;
t.update({_id: 0}, {$set: {_id: 1}});
assert(db.getLastError());
assert.eq(opCounters.update + 1, db.serverStatus().opcounters.update);

//
// 3. Delete.
//
// - counted as 1 op, regardless of errors
//

t.drop();
t.insert([{_id: 0}, {_id: 1}]);

// Delete, no error.
opCounters = db.serverStatus().opcounters;
t.remove({_id: 0});
assert(!db.getLastError());
assert.eq(opCounters.delete + 1, db.serverStatus().opcounters.delete);

// Delete, with error.
opCounters = db.serverStatus().opcounters;
t.remove({_id: {$invalidOp: 1}});
assert(db.getLastError());
assert.eq(opCounters.delete + 1, db.serverStatus().opcounters.delete);

//
// 4. Query.
//
// - mongod: counted as 1 op, regardless of errors
// - mongos: counted as 1 op if successful, else 0
//

t.drop();
t.insert({_id: 0});

// Query, no error.
opCounters = db.serverStatus().opcounters;
t.findOne();
assert.eq(opCounters.query + 1, db.serverStatus().opcounters.query);

// Query, with error.
opCounters = db.serverStatus().opcounters;
assert.throws(function() {
    t.findOne({_id: {$invalidOp: 1}});
});
assert.eq(opCounters.query + (isMongos ? 0 : 1), db.serverStatus().opcounters.query);

//
// 5. Getmore.
//
// - counted as 1 op per getmore issued, regardless of errors
//

t.drop();
t.insert([{_id: 0}, {_id: 1}, {_id: 2}]);

// Getmore, no error.
opCounters = db.serverStatus().opcounters;
t.find().batchSize(2).toArray();  // 3 documents, batchSize=2 => 1 query + 1 getmore
assert.eq(opCounters.query + 1, db.serverStatus().opcounters.query);
assert.eq(opCounters.getmore + 1, db.serverStatus().opcounters.getmore);

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
serverStatus = db.runCommand({serverStatus: 1});
opCounters = serverStatus.opcounters;
metricsObj = serverStatus.metrics.commands;
assert.eq(opCounters.command + 1, db.serverStatus().opcounters.command);  // "serverStatus" counted
// Count this and the last run of "serverStatus"
assert.eq(metricsObj.serverStatus.total + 2,
          db.serverStatus().metrics.commands.serverStatus.total,
          "total ServerStatus command counter did not increment");
assert.eq(metricsObj.serverStatus.failed,
          db.serverStatus().metrics.commands.serverStatus.failed,
          "failed ServerStatus command counter incremented!");

// Command, recognized, with error.
serverStatus = db.runCommand({serverStatus: 1});
opCounters = serverStatus.opcounters;
metricsObj = serverStatus.metrics.commands;
var countVal = {"total": 0, "failed": 0};
if (metricsObj.count != null) {
    countVal = metricsObj.count;
}
res = t.runCommand("count", {query: {$invalidOp: 1}});
assert.eq(0, res.ok);
assert.eq(opCounters.command + 2,
          db.serverStatus().opcounters.command);  // "serverStatus", "count" counted

assert.eq(countVal.total + 1,
          db.serverStatus().metrics.commands.count.total,
          "total count command counter did not incremented");
assert.eq(countVal.failed + 1,
          db.serverStatus().metrics.commands.count.failed,
          "failed count command counter did not increment");

// Command, unrecognized.
serverStatus = db.runCommand({serverStatus: 1});
opCounters = serverStatus.opcounters;
metricsObj = serverStatus.metrics.commands;
res = t.runCommand("invalid");
assert.eq(0, res.ok);
assert.eq(opCounters.command + 1, db.serverStatus().opcounters.command);  // "serverStatus" counted
assert.eq(null, db.serverStatus().metrics.commands.invalid);
assert.eq(metricsObj['<UNKNOWN>'] + 1, db.serverStatus().metrics.commands['<UNKNOWN>']);
