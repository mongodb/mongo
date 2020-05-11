/**
 * Tests the explain command with the maxTimeMS option.
 */
(function() {
"use strict";

const standalone = MongoRunner.runMongod();
assert.neq(null, standalone, "mongod was unable to start up");

const dbName = "test";
const db = standalone.getDB(dbName);
const collName = "explain_max_time_ms";
const coll = db.getCollection(collName);

const destCollName = "explain_max_time_ms_dest";
const mapFn = function() {
    emit(this.i, this.j);
};
const reduceFn = function(key, values) {
    return Array.sum(values);
};

coll.drop();
assert.commandWorked(db.createCollection(collName));

assert.commandWorked(coll.insert({i: 1, j: 1}));
assert.commandWorked(coll.insert({i: 2, j: 1}));
assert.commandWorked(coll.insert({i: 2, j: 2}));

// Set fail point to make sure operations with "maxTimeMS" set will time out.
assert.commandWorked(
    db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}));

for (const verbosity of ["executionStats", "allPlansExecution"]) {
    // Expect explain to time out if "maxTimeMS" is set on the aggregate command.
    assert.commandFailedWithCode(assert.throws(function() {
                                                  coll.explain(verbosity).aggregate(
                                                      [{$match: {i: 1}}], {maxTimeMS: 1});
                                              }),
                                              ErrorCodes.MaxTimeMSExpired);
    // Expect explain to time out if "maxTimeMS" is set on the count command.
    assert.commandFailedWithCode(assert.throws(function() {
                                                  coll.explain(verbosity).count({i: 1},
                                                                                {maxTimeMS: 1});
                                              }),
                                              ErrorCodes.MaxTimeMSExpired);
    // Expect explain to time out if "maxTimeMS" is set on the distinct command.
    assert.commandFailedWithCode(assert.throws(function() {
                                                  coll.explain(verbosity).distinct(
                                                      "i", {}, {maxTimeMS: 1});
                                              }),
                                              ErrorCodes.MaxTimeMSExpired);
    // Expect explain to time out if "maxTimeMS" is set on the find command.
    assert.commandFailedWithCode(assert.throws(function() {
                                                  coll.find().maxTimeMS(1).explain(verbosity);
                                              }),
                                              ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(
        assert.throws(function() {
                         coll.explain(verbosity).find().maxTimeMS(1).finish();
                     }),
                     ErrorCodes.MaxTimeMSExpired);
    // Expect explain to time out if "maxTimeMS" is set on the findAndModify command.
    assert.commandFailedWithCode(assert.throws(function() {
                                                  coll.explain(verbosity).findAndModify(
                                                      {update: {$inc: {j: 1}}, maxTimeMS: 1});
                                              }),
                                              ErrorCodes.MaxTimeMSExpired);
}

// Disable fail point.
assert.commandWorked(db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}));

MongoRunner.stopMongod(standalone);
})();
