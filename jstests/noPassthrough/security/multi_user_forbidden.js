/**
 * Tests that apiStrict forbids authentication as multiple users.
 * @tags: [requires_auth]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function runTest(conn) {
    const db1 = "foo";
    const user1 = "alice";

    const db2 = "bar";
    const user2 = "bob";

    const db3 = "foo";
    const user3 = "carol";

    const pass = "pwd";

    conn.getDB(db1).createUser({user: user1, pwd: pass, roles: []});
    conn.getDB(db2).createUser({user: user2, pwd: pass, roles: []});
    conn.getDB(db3).createUser({user: user3, pwd: pass, roles: []});

    {
        jsTest.log("Testing the rainbow of auth with a vanilla connection");

        const vanillaConn = new Mongo(conn.host);
        assert(vanillaConn.getDB(db1).auth(user1, pass), "Initial authN should succeed");
        assert(!vanillaConn.getDB(db2).auth(user2, pass), "AuthN on another db should not succeed");
        assert(vanillaConn.getDB(db1).auth(user1, pass), "Re-authN as first user should succeed");
        assert(
            !vanillaConn.getDB(db3).auth(user3, pass),
            "AuthN as a new user on the first database should not succeed",
        );

        vanillaConn.getDB(db1).logout();
    }

    {
        jsTest.log("Testing the rainbow of auth with an { apiStrict: false } connection");

        const laxConn = new Mongo(conn.host, undefined, {api: {version: "1", strict: false}});
        assert(laxConn.getDB(db1).auth(user1, pass), "Initial authN should succeed");
        assert(!laxConn.getDB(db2).auth(user2, pass), "AuthN on another db should not succeed");
        assert(laxConn.getDB(db1).auth(user1, pass), "Re-authN as first user should succeed");
        assert(!laxConn.getDB(db3).auth(user3, pass), "AuthN as a new user on the first database should not succeed");

        laxConn.getDB(db1).logout();
    }

    {
        jsTest.log("Testing the rainbow of auth with an { apiStrict: true } connection");

        const strictConn = new Mongo(conn.host, undefined, {api: {version: "1", strict: true}});
        assert(strictConn.getDB(db1).auth(user1, pass), "Initial authN should succeed");
        assert(!strictConn.getDB(db2).auth(user2, pass), "AuthN on another db should fail");
        assert(!strictConn.getDB(db1).auth(user1, pass), "Re-authN as first user should fail");
        assert(!strictConn.getDB(db3).auth(user3, pass), "AuthN as a new user on the first database should fail");

        strictConn.getDB(db1).logout();
    }

    {
        jsTest.log(
            "Testing the rainbow of auth with an { apiStrict: true } connection " +
                "and the allowMultipleUsersWithApiStrict fail point",
        );

        const fp = configureFailPoint(conn, "allowMultipleUsersWithApiStrict");
        const strictishConn = new Mongo(conn.host, undefined, {api: {version: "1", strict: true}});

        assert(strictishConn.getDB(db1).auth(user1, pass), "Initial authN should succeed");
        assert(!strictishConn.getDB(db2).auth(user2, pass), "AuthN on another db should not succeed");
        assert(strictishConn.getDB(db1).auth(user1, pass), "Re-authN as first user should succeed");
        assert(
            !strictishConn.getDB(db3).auth(user3, pass),
            "AuthN as a new user on the first database should not succeed",
        );

        strictishConn.getDB(db1).logout();
        fp.off();
    }
}

{
    const conn = MongoRunner.runMongod();

    runTest(conn);

    MongoRunner.stopMongod(conn);
}
