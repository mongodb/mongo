/**
 * Tests that 'create' rejects a clusteredIndex whose 'name' is empty or contains an embedded null
 * byte.
 */
const conn = MongoRunner.runMongod();
const testDB = conn.getDB(jsTestName());

const nullByte = String.fromCharCode(0);

let collCounter = 0;
const nextCollName = () => `clusteredColl${collCounter++}`;

// The empty-name and null-byte rejections share an error code, so each caller states which one it
// expects.
function assertNameRejected(name, expectedReason) {
    const collName = nextCollName();
    const res = testDB.createCollection(collName, {
        clusteredIndex: {key: {_id: 1}, unique: true, name},
    });
    assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
    assert(res.errmsg.includes(expectedReason),
           `rejected for the wrong reason: ${tojson({
               errmsg: res.errmsg,
               expectedReason,
               nameLength: name.length
           })}`);
    assert(!testDB.getCollectionNames().includes(collName),
           `bad create command left a collection behind: ${collName}`);
}

const failReasonNullByte = "name cannot contain NUL bytes";
const failReasonEmptyName = "name cannot be empty";

// A single null byte.
assertNameRejected(nullByte, failReasonNullByte);

// Two null bytes.
assertNameRejected(nullByte.repeat(2), failReasonNullByte);

// A null byte embedded among other characters.
assertNameRejected(`leading${nullByte}trailing`, failReasonNullByte);

// A leading null byte.
assertNameRejected(`${nullByte}trailing`, failReasonNullByte);

// A trailing null byte.
assertNameRejected(`leading${nullByte}`, failReasonNullByte);

// An empty name.
assertNameRejected("", failReasonEmptyName);

// A valid name is still accepted.
const validCollName = nextCollName();
assert.commandWorked(testDB.createCollection(validCollName, {
    clusteredIndex: {key: {_id: 1}, unique: true, name: "myClusteredIndex"},
}));
assert.commandWorked(testDB.runCommand({collStats: validCollName}));

MongoRunner.stopMongod(conn);
