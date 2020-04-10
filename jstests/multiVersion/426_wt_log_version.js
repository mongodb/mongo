/**
 * 4.2.6+ has the following capabilities:
 * - It can read data left behind by 4.4.
 * - It can read data left behind by 4.2.5-
 * - It can read/write log version 3 (used by 4.2.5-)
 * - It can read/write log version 4 (only known by 4.2.6+)
 *
 * This test ensures that a 4.2.5- ("4.2 classic") version can binary upgrade to 4.2.6+ and
 * binary downgrade back to a 4.2 classic version.
 *
 * This test ensures that a clean database started on a 4.2.6+ binary can binary downgrade to a 4.2
 * classic version.
 */
(function() {
"use strict";
// Chosen only because other multiversion tests target 4.2.1.
const classic42 = "4.2.1";

// Test the following binary version changes succeeds: classic -> 4.2.6+ -> classic
{
    let conn = MongoRunner.runMongod({cleanData: true, binVersion: classic42});
    assert.neq(null, conn, "mongod-" + classic42 + " failed to start up");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({restart: conn, noCleanData: true, binVersion: "4.2"});
    assert.neq(null, conn, "mongod-4.2-latest failed to start up");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({restart: conn, noCleanData: true, binVersion: classic42});
    assert.neq(null, conn, "mongod-" + classic42 + " failed to start up after 4.2 shutdown");
    MongoRunner.stopMongod(conn);
}

// Test that a new dbpath on 4.2.6+ can binary downgrade to 4.2 classic.
{
    let conn = MongoRunner.runMongod({cleanData: true, binVersion: "4.2"});
    assert.neq(null, conn, "mongod-4.2-latest failed to start up");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({restart: conn, noCleanData: true, binVersion: classic42});
    assert.neq(null, conn, "mongod-" + classic42 + " failed to start up");
    MongoRunner.stopMongod(conn);
}
})();
