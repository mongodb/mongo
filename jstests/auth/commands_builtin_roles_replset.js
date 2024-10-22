import {createUsers, roles, runOneTest} from "jstests/auth/lib/commands_builtin_roles.js";
import {authCommandsLib} from "jstests/auth/lib/commands_lib.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

function isPrimaryNode(conn) {
    return (conn.adminCommand({hello: 1}).isWritablePrimary);
}

function updateRolesWithPrimaryDB(primaryConn) {
    roles.forEach(role => {
        role.db = primaryConn.getDB(role.dbname);
    });
}

function runAllCommandsBuiltinRoles(rst) {
    // Tracks the connection 'conn' and ensures it is connecting to writable primary.
    let conn = rst.getPrimary();

    const runOneTestOnPrimary = (_, t) => {
        // Some test commands may make 'conn' step down as a secondary. If that happens, update
        // 'conn' with a primary one.
        if (!isPrimaryNode(conn)) {
            conn = rst.getPrimary();
            // The DBs in 'roles' may still connect to a secondary. Update them with the DBs from
            // 'conn'.
            updateRolesWithPrimaryDB(conn);
        }
        return runOneTest(conn, t);
    };

    const testFunctionImpls = {createUsers: createUsers, runOneTest: runOneTestOnPrimary};
    authCommandsLib.runTests(conn, testFunctionImpls);
}

let mongotmock;
let mongotHost = "localhost:27017";
if (!_isWindows()) {
    mongotmock = new MongotMock();
    mongotmock.start();
    mongotHost = mongotmock.getConnection().host;
}

const dbPath = MongoRunner.toRealDir("$dataDir/commands_built_in_roles_replset/");
mkdir(dbPath);
const opts = {
    auth: "",
    setParameter: {
        trafficRecordingDirectory: dbPath,
        mongotHost,   // We have to set the mongotHost parameter for the
                      // $search-relatead tests to pass configuration checks.
        syncdelay: 0  // Disable checkpoints as this can cause some commands to fail transiently.
    }
};

// run all tests replset
const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: opts,
    waitForKeys: false,
    keyFile: "jstests/libs/key1",
});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

runAllCommandsBuiltinRoles(rst);
rst.stopSet();

if (mongotmock) {
    mongotmock.stop();
}
