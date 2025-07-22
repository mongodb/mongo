/*
 * Verify that internal system queries succeed when histogramCE is enabled. This is a regression
 * test for SERVER-107736.
 */

const conn = MongoRunner.runMongod({setParameter: {planRankerMode: "histogramCE"}});

const db = conn.getDB("admin");
assert.commandWorked(db.adminCommand({createUser: "testUser", pwd: "pwd", roles: []}));
// The auth() command causes a system query to be run, which should succeed.
assert(db.auth("testUser", "pwd"));

MongoRunner.stopMongod(conn);
