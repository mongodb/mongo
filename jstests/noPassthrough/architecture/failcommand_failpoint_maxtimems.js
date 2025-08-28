let conn = MongoRunner.runMongod();
let admin = conn.getDB("admin");
var db = conn.getDB("test");

let fpCmd = {
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        failCommands: ["insert"],
        blockConnection: true,
        blockTimeMS: 1000,
    },
};

assert.commandWorked(admin.runCommand(fpCmd));

let insertCmd = {
    insert: "coll",
    documents: [{x: 1}],
    maxTimeMS: 100,
};

assert.commandFailedWithCode(db.runCommand(insertCmd), ErrorCodes.MaxTimeMSExpired);

MongoRunner.stopMongod(conn);
