/**
 * Once data files are touched by a 4.4 binary, they can only downgrade to 4.2.6+. Even after a
 * successful downgrade to 4.2.6+, they can never be downgraded to an earlier maintenance version,
 * nor minor version.
 */
(function() {
"use strict";

let conn = MongoRunner.runMongod({binVersion: "latest"});
assert.neq(null, conn, "mongod was unable to start up");
let adminDB = conn.getDB("admin");
checkFCV(adminDB, "4.4");
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.2"}));

MongoRunner.stopMongod(conn);
assert.eq(MongoRunner.EXIT_ABRUPT,
          runMongoProgram("mongod-4.2.1", "--port", conn.port, "--dbpath", conn.dbpath));

conn = MongoRunner.runMongod({restart: conn, noCleanData: true, binVersion: "4.2"});
assert.neq(null, conn, "mongod-4.2-latest failed to start up");
MongoRunner.stopMongod(conn);

assert.eq(MongoRunner.EXIT_ABRUPT,
          runMongoProgram("mongod-4.2.1", "--port", conn.port, "--dbpath", conn.dbpath));
})();
