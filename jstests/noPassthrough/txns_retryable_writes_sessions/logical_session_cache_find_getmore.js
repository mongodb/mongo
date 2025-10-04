TestData.disableImplicitSessions = true;

let conn = MongoRunner.runMongod({setParameter: {maxSessions: 2}});
let testDB = conn.getDB("test");

assert.commandWorked(testDB.foo.insert({data: 1}));
assert.commandWorked(testDB.foo.insert({data: 2}));

for (let i = 0; i < 2; i++) {
    let session = conn.startSession();
    var db = session.getDatabase("test");
    let res = assert.commandWorked(
        db.runCommand({find: "foo", batchSize: 1}),
        "unable to run find when the cache is not full",
    );
    let cursorId = res.cursor.id;
    assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: "foo"}),
        "unable to run getMore when the cache is not full",
    );
}

let session3 = conn.startSession();
var db = session3.getDatabase("test");
assert.commandFailed(db.runCommand({find: "foo", batchSize: 1}), "able to run find when the cache is full");

MongoRunner.stopMongod(conn);
