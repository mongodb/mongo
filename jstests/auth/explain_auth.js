// Test auth of the explain command.

let conn = MongoRunner.runMongod({auth: ""});

let admin = conn.getDB("admin");
admin.createUser({user: "adminUser", pwd: "pwd", roles: ["root"]});
admin.auth({user: "adminUser", pwd: "pwd"});

var db = conn.getDB("explain_auth_db");
let coll = db.explain_auth_coll;

assert.commandWorked(coll.insert({_id: 1, a: 1}));

/**
 * Runs explains of find, count, remove, and update. Checks that they either succeed or fail with
 * "not authorized".
 *
 * Takes as input a document 'authSpec' with the following format:
 *   {
 *      find: <bool>,
 *      count: <bool>,
 *      remove: <bool>,
 *      update: <bool>
 *   }
 *
 * A true value indicates that the corresponding explain should succeed, whereas a false value
 * indicates that the explain command should not be authorized.
 */
function testExplainAuth(authSpec) {
    let cmdResult;

    function assertCmdResult(result, expectSuccess) {
        if (expectSuccess) {
            assert.commandWorked(result);
        } else {
            assert.commandFailedWithCode(result, 13);
        }
    }

    // .find()
    cmdResult = db.runCommand({explain: {find: coll.getName()}});
    assertCmdResult(cmdResult, authSpec.find);

    // .count()
    cmdResult = db.runCommand({explain: {count: coll.getName()}});
    assertCmdResult(cmdResult, authSpec.count);

    // .remove()
    cmdResult = db.runCommand({explain: {delete: coll.getName(), deletes: [{q: {a: 1}, limit: 1}]}});
    assertCmdResult(cmdResult, authSpec.remove);

    // .update()
    cmdResult = db.runCommand({explain: {update: coll.getName(), updates: [{q: {a: 1}, u: {$set: {b: 1}}}]}});
    assertCmdResult(cmdResult, authSpec.update);
}

// Create some user-defined roles which we will grant to the users below.
db.createRole({
    role: "findOnly",
    privileges: [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["find"]}],
    roles: [],
});
db.createRole({
    role: "updateOnly",
    privileges: [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["update"]}],
    roles: [],
});
db.createRole({
    role: "removeOnly",
    privileges: [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["remove"]}],
    roles: [],
});

// Create three users:
//  -- user defined role with just "find"
//  -- user defined role with just "update"
//  -- user defined role with just "remove"
db.createUser({user: "findOnly", pwd: "pwd", roles: ["findOnly"]});
db.createUser({user: "updateOnly", pwd: "pwd", roles: ["updateOnly"]});
db.createUser({user: "removeOnly", pwd: "pwd", roles: ["removeOnly"]});

// We're done doing test setup, so admin user should no longer be logged in.
admin.logout();

// The "find" action allows explain of read operations.
db.auth("findOnly", "pwd");
testExplainAuth({find: true, count: true, remove: false, update: false});
db.logout();

db.auth("updateOnly", "pwd");
testExplainAuth({find: false, count: false, remove: false, update: true});
db.logout();

db.auth("removeOnly", "pwd");
testExplainAuth({find: false, count: false, remove: true, update: false});
db.logout();

MongoRunner.stopMongod(conn, null, {user: "adminUser", pwd: "pwd"});
