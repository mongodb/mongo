// Test mapReduce use with different values of the allowDiskUseByDefault parameter.

(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
const coll = db.getCollection(jsTestName());
coll.drop();

const memoryLimitMb = 1;
const largeStr = "A".repeat(1024 * 1024);  // 1MB string

// Create a collection exceeding the memory limit.
for (let i = 0; i < memoryLimitMb + 1; ++i)
    assert.commandWorked(coll.insert({largeStr: largeStr}));

const mapReduceCmd = {
    mapReduce: coll.getName(),
    map: function() {
        emit("a", this.largeStr);
    },
    reduce: function(k, v) {
        return 42;
    },
    out: {inline: 1}
};

assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: memoryLimitMb * 1024 * 1024}));

assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: false}));
assert.commandFailedWithCode(db.runCommand(mapReduceCmd),
                             ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: true}));
const res = assert.commandWorked(db.runCommand(mapReduceCmd));
assert.eq(res.results[0], {_id: "a", value: 42}, res);

MongoRunner.stopMongod(conn);
})();
