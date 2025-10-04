import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({name: "dbhash_system_collections", nodes: 2});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();

let testDB = primary.getDB("test");
assert.commandWorked(testDB.system.users.insert({users: 1}));
assert.commandWorked(testDB.system.js.insert({js: 1}));

let adminDB = primary.getDB("admin");
assert.commandWorked(adminDB.system.roles.insert({roles: 1}));
assert.commandWorked(adminDB.system.version.insert({version: 1}));
assert.commandWorked(adminDB.system.backup_users.insert({backup_users: 1}));

rst.awaitReplication();

function checkDbHash(mongo) {
    let testDB = mongo.getDB("test");
    let adminDB = mongo.getDB("admin");

    let replicatedSystemCollections = ["system.js", "system.users"];

    let replicatedAdminSystemCollections = ["system.backup_users", "system.keys", "system.roles", "system.version"];

    let res = testDB.runCommand("dbhash");
    assert.commandWorked(res);
    assert.docEq(replicatedSystemCollections, Object.keys(res.collections), tojson(res));

    res = adminDB.runCommand("dbhash");
    assert.commandWorked(res);
    assert.docEq(replicatedAdminSystemCollections, Object.keys(res.collections), tojson(res));

    return res.md5;
}

let primaryMd5 = checkDbHash(primary);
let secondaryMd5 = checkDbHash(secondary);
assert.eq(primaryMd5, secondaryMd5, "dbhash is different on the primary and the secondary");
rst.stopSet();
