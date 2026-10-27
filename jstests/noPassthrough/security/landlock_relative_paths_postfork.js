/**
 * Verifies the Landlock filesystem policy on a --fork'd server whose paths are configured
 * *relative*, which is the only configuration in which the policy's two resolution bases (see
 * PathBase in src/mongo/db/landlock_init.h) can disagree.
 * Without --fork the two name the same directory and a mistake is invisible; with --fork a rule
 * built on the wrong base binds to a path the server never opens, which denies the access and,
 * for anything opened at startup, kills the server.
 * Nodes run with landlockMode "bestEffort"; on a kernel without Landlock they simply start
 * unsandboxed, and the enforcement-specific assertions are skipped. Any failure to apply the
 * policy on a kernel that does support Landlock is fatal in the server, so a mis-derived rule
 * shows up as a node that fails to start.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    pathRelativeToDaemonCwd,
    pathRelativeToStartupCwd,
    getLandlockRootRelativeScratchDirectory,
    startForkedMongod,
    stopForkedMongod,
    assertLandlockMode,
} from "jstests/noPassthrough/security/landlock_helpers.js";
import {isPlatformCompatibleWithExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

if (getBuildInfo().buildEnvironment.target_os !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test");
    quit();
}

// The shell's cwd which every "pre-fork" relative option is resolved against.
const kCwd = pwd();

// Scratch hierarchy for the paths resolved against the launcher's cwd.
const kRoot = MongoRunner.dataPath + jsTestName();
resetDbpath(kRoot);

function assertLandlockStatus(conn) {
    return assertLandlockMode(conn, "bestEffort");
}

/** Restricts `path` to owner-only access, which the extension loader requires of its inputs. */
function restrictToOwner(path, mode) {
    assert.eq(0, runNonMongoProgram("chmod", mode, path), `failed to chmod ${path} to ${mode}`);
}

describe("relative paths with --fork", function () {
    // Paths the server resolves against the cwd the launcher was started in.
    const mainRoot = `${kRoot}/main`;
    const dbDir = `${mainRoot}/db`;
    const logDir = `${mainRoot}/log`;
    const logFile = `${logDir}/mongod.log`;
    const backtraceFile = `${logDir}/backtrace.log`;

    // Paths the daemon resolves against its own cwd of "/".
    const rootRelRoot = getLandlockRootRelativeScratchDirectory("main");
    const rootRelDirs = {
        run: `${rootRelRoot}/run`,
        socket: `${rootRelRoot}/socket`,
        ftdc: `${rootRelRoot}/ftdc`,
        tls: `${rootRelRoot}/tls`,
        proxySocket: `${rootRelRoot}/proxy`,
    };
    const pidFile = `${rootRelDirs.run}/mongod.pid`;
    // opensslDiffieHellmanParameters is a setParameter, so unlike --tlsCAFile and friends it is
    // never made absolute at parse time and is read with the daemon's own cwd.
    const dhParamFile = `${rootRelDirs.tls}/dhparam.pem`;

    // server startup fails if this timeZoneInfo file is inaccessible
    const tzDir = `${kCwd}/jstests/libs/config_files/good_timezone_info_slim`;

    const port = allocatePort();
    let conn;
    let daemonPid;

    before(function () {
        mkdir(dbDir);
        mkdir(logDir);
        for (const dir of Object.values(rootRelDirs)) {
            mkdir(dir);
        }
        copyFile("jstests/libs/8k-prime.dhparam", dhParamFile);

        ({conn, pid: daemonPid} = startForkedMongod({
            port: port,
            fork: "",
            auth: "",
            landlockMode: "bestEffort",
            // Made absolute before the daemon chdir()s, so relative to the launcher's cwd.
            dbpath: pathRelativeToStartupCwd(dbDir),
            logpath: pathRelativeToStartupCwd(logFile),
            keyFile: "jstests/libs/key1",
            tlsMode: "allowTLS",
            tlsCertificateKeyFile: "jstests/libs/server.pem",
            tlsClusterFile: "jstests/libs/cluster_cert.pem",
            tlsCAFile: "jstests/libs/ca.pem",
            tlsClusterCAFile: "jstests/libs/ca.pem",
            tlsCRLFile: "jstests/libs/crl.pem",
            // Kept verbatim and opened with the daemon's cwd, so relative to "/".
            pidfilepath: pathRelativeToDaemonCwd(pidFile),
            unixSocketPrefix: pathRelativeToDaemonCwd(rootRelDirs.socket),
            proxyUnixSocketPrefix: pathRelativeToDaemonCwd(rootRelDirs.proxySocket),
            timeZoneInfo: pathRelativeToDaemonCwd(tzDir),
            setParameter: {
                diagnosticDataCollectionDirectoryPath: pathRelativeToDaemonCwd(rootRelDirs.ftdc),
                opensslDiffieHellmanParameters: pathRelativeToDaemonCwd(dhParamFile),
            },
        }));
    });

    it("verify server access to relative paths", function () {
        // The log file and the data directory are resolved against the cwd the launcher ran in, so
        // they must appear there and not under the daemon's "/".
        assert.soon(
            () => pathExists(logFile),
            `the forked daemon's log file is not at ${logFile}, where the relative logpath ` +
                `resolves against the cwd the launcher was started in`,
        );
        assert.soon(
            () => listFiles(dbDir).length > 0,
            `the forked daemon wrote no data files under ${dbDir}`,
        );

        // The pid file, socket and FTDC directory are opened with the daemon's own cwd, so they
        // must appear under "/" -- and the policy had to grant them there, derived the same way.
        assert.soon(
            () => pathExists(pidFile),
            `pid file was not created at the root-relative pidfilepath ${pidFile}`,
        );
        assert.soon(
            () => pathExists(`${rootRelDirs.socket}/mongodb-${port}.sock`),
            `UNIX domain socket was not created under the root-relative unixSocketPrefix ` +
                rootRelDirs.socket,
        );
        assert.soon(
            () => listFiles(rootRelDirs.ftdc).length > 0,
            `FTDC wrote nothing under the root-relative diagnostic data directory ` +
                rootRelDirs.ftdc,
        );
        assert.soon(
            () => pathExists(`${rootRelDirs.proxySocket}/proxy-mongodb-${port}.sock`),
            `no proxy socket was created under the root-relative proxyUnixSocketPrefix ` +
                rootRelDirs.proxySocket,
        );

        // Assert keyfile auth & user creation work
        const admin = conn.getDB("admin");
        authutil.asCluster(conn, "jstests/libs/key1", () => {
            admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        });
        admin.auth("admin", "pwd");

        // Assert serverStatus, normal writes, log & ssl rotations work
        assertLandlockStatus(conn);
        assert.commandWorked(conn.getDB("foo")["bar"].insert({_id: 0, x: 1}));
        assert.commandWorked(conn.adminCommand({logRotate: 1}));
        assert.commandWorked(conn.adminCommand({rotateCertificates: 1}));

        // Assert basic networking works
        assert.commandWorked(conn.adminCommand({ping: 1}));
        assert.eq(
            0,
            runMongoProgram(
                "mongo",
                "--host",
                "localhost",
                "--port",
                port,
                "-u",
                "admin",
                "-p",
                "pwd",
                "--tls",
                "--tlsCAFile",
                "jstests/libs/ca.pem",
                "--tlsCertificateKeyFile",
                "jstests/libs/client.pem",
                "--eval",
                ";",
            ),
        );

        admin.logout();
    });

    after(function () {
        stopForkedMongod(conn, daemonPid);
        // Outside the test's data directory, so resmoke will not clean it up.
        removeFile(rootRelRoot);
    });
});

// Traffic recording gets its own test setup because its directory cannot be configured consistently
// alongside the rest. trafficRecordingDirectory's validator resolves the path before fork
// against the launcher's cwd, whereas the traffic recorder later resolves it, post-fork, against
// the current cwd. A relative value therefore has to name a directory that exists under
// *both* bases, or the node will not start.
// TODO: SERVER-134689 adjust this test once the bug described above is fixed.
describe("relative traffic recording directory with --fork", function () {
    const trafficRoot = `${kRoot}/traffic`;
    const dbDir = `${trafficRoot}/db`;
    const logFile = `${trafficRoot}/mongod.log`;

    const rootRelRoot = getLandlockRootRelativeScratchDirectory("traffic");
    const trafficDir = `${rootRelRoot}/recordings`;
    // The same relative string, resolved against the launcher's cwd instead, for the validator.
    const mirrorDir = `${kCwd}/${pathRelativeToDaemonCwd(trafficDir)}`;

    const port = allocatePort();
    let conn;
    let daemonPid;

    before(function () {
        mkdir(dbDir);
        mkdir(trafficDir);
        mkdir(mirrorDir);

        ({conn, pid: daemonPid} = startForkedMongod({
            port: port,
            fork: "",
            landlockMode: "bestEffort",
            dbpath: pathRelativeToStartupCwd(dbDir),
            logpath: pathRelativeToStartupCwd(logFile),
            pidfilepath: `${trafficRoot}/mongod.pid`,
            setParameter: {trafficRecordingDirectory: pathRelativeToDaemonCwd(trafficDir)},
        }));
    });

    it("records traffic under the directory the daemon's cwd resolves to", function () {
        assertLandlockStatus(conn);

        const recording = `${jsTestName()}-recording`;
        assert.commandWorked(conn.adminCommand({startTrafficRecording: 1, destination: recording}));
        assert.commandWorked(conn.adminCommand({stopTrafficRecording: 1}));

        assert.soon(
            () => pathExists(`${trafficDir}/${recording}`),
            `traffic recording wrote nothing under ${trafficDir}`,
        );
    });

    after(function () {
        stopForkedMongod(conn, daemonPid);
        removeFile(rootRelRoot);
        removeFile(mirrorDir);
    });
});

// The extension rules are derived in two steps -- a grant on the config directory, then a grant on
// each sharedLibraryPath (and its ".sig") read out of the .conf files in it -- so both the
// directory and the libraries it points at are configured relative here. The test builds its own
// config directory rather than using the resmoke-generated one under /tmp, since the policy grants
// /tmp statically and a mis-derived rule would be masked there.
//
// processManagement.extensionsSignaturePublicKeyPath is not covered: its rule is compiled out of
// builds that verify signatures (MONGO_CONFIG_EXT_SIG_SECURE).
describe("relative extension paths with --fork", function () {
    const extensionsRoot = `${kRoot}/extensions`;
    const dbDir = `${extensionsRoot}/db`;
    const logFile = `${extensionsRoot}/mongod.log`;

    const rootRelRoot = getLandlockRootRelativeScratchDirectory("extensions");
    const confDir = `${rootRelRoot}/conf`;
    const libDir = `${rootRelRoot}/lib`;
    const soName = "libfoo_mongo_extension.so";
    const extensionName = `foo_${UUID().hex()}`;
    const soFile = `${libDir}/${soName}`;

    // the same relative string, resolved against the launcher's cwd instead, for the validator.
    const mirrorDir = `${kCwd}/${pathRelativeToDaemonCwd(confDir)}`;
    const port = allocatePort();
    let conn;
    let daemonPid;

    before(function () {
        if (!isPlatformCompatibleWithExtensions()) {
            jsTest.log.info("extensions are not supported on this platform; skipping");
            return;
        }
        const sourceSo = MongoRunner.getExtensionPath(soName);
        if (!pathExists(sourceSo)) {
            jsTest.log.info("extension library not present in this build; skipping", {sourceSo});
            return;
        }

        mkdir(dbDir);
        mkdir(confDir);
        mkdir(libDir);
        mkdir(mirrorDir);
        copyFile(sourceSo, soFile);

        // The library is named relative to "/" in the .conf too: the loader opens whatever string
        // it finds there with the daemon's cwd, and the policy derives its grant the same way.
        writeFile(
            `${confDir}/${extensionName}.conf`,
            `sharedLibraryPath: ${pathRelativeToDaemonCwd(soFile)}\n`,
        );
        // The server rejects a config directory or file that is group- or other-writable.
        restrictToOwner(confDir, "700");
        restrictToOwner(`${confDir}/${extensionName}.conf`, "600");

        ({conn, pid: daemonPid} = startForkedMongod({
            port: port,
            fork: "",
            landlockMode: "bestEffort",
            dbpath: pathRelativeToStartupCwd(dbDir),
            logpath: pathRelativeToStartupCwd(logFile),
            pidfilepath: `${extensionsRoot}/mongod.pid`,
            extensionsConfigPath: pathRelativeToDaemonCwd(confDir),
            loadExtensions: extensionName,
        }));
    });

    it("loads an extension named by root-relative paths under the sandbox", function () {
        if (!conn) {
            return; // Unsupported platform or build; the before hook logged why.
        }
        // Reaching this point is most of the assertion: the config directory is read and the
        // library dlopen()ed after the sandbox engages, so a rule derived against the wrong cwd
        // takes the node down before it serves.
        assertLandlockStatus(conn);

        const kExtensionLoadedId = 10668503;
        assert.soon(
            () => cat(logFile).includes(`"id":${kExtensionLoadedId}`),
            `query extension did not get loaded from ${soFile}`,
        );
    });

    after(function () {
        stopForkedMongod(conn, daemonPid);
        removeFile(rootRelRoot);
        removeFile(mirrorDir);
    });
});

// The enterprise policy's audit log grant is derived the same way the community log-file grant is
// -- resolved against the pre-fork cwd, because the audit manager resolves auditLog.path with
// serverGlobalParams.cwd -- and, like it, is opened after the sandbox engages. It is also the one
// enterprise path a node can be given without standing up an external service (a KMIP or LDAP
// server) or shipping a plugin, so it is the one covered here; the rest of the enterprise rules
// are exercised by their features' own tests.
describe("enterprise audit log with a relative path and --fork", function () {
    const auditRoot = `${kRoot}/audit`;
    const dbDir = `${auditRoot}/db`;
    const logFile = `${auditRoot}/mongod.log`;
    // Deliberately one level below a directory that does not exist yet: the audit manager creates
    // its log's directory when missing, so the grant has to land on the parent.
    const auditFile = `${auditRoot}/created-by-the-audit-manager/audit.json`;
    const port = allocatePort();

    let conn;
    let daemonPid;

    before(function () {
        if (!getBuildInfo().modules.includes("enterprise")) {
            jsTest.log.info("not an enterprise build; skipping the audit log case");
            return;
        }
        mkdir(dbDir);

        ({conn, pid: daemonPid} = startForkedMongod({
            port: port,
            fork: "",
            landlockMode: "bestEffort",
            dbpath: pathRelativeToStartupCwd(dbDir),
            logpath: pathRelativeToStartupCwd(logFile),
            pidfilepath: `${auditRoot}/mongod.pid`,
            auditDestination: "file",
            auditFormat: "JSON",
            auditPath: pathRelativeToStartupCwd(auditFile),
        }));
    });

    it("sandboxes the daemon and writes the audit log where the relative path resolves", function () {
        if (!conn) {
            return; // Community build; the before hook logged why.
        }
        assertLandlockStatus(conn);
        assert.soon(
            () => pathExists(auditFile),
            `the audit log is not at ${auditFile}, where the relative auditPath resolves ` +
                `against the pre-fork cwd`,
        );

        // Audit records are written as events happen, so a command must land in the file.
        const sizeBefore = cat(auditFile).length;
        assert.commandWorked(conn.getDB(jsTestName()).createCollection("audited"));
        assert.soon(
            () => cat(auditFile).length > sizeBefore,
            `the audit log at ${auditFile} did not grow after an audited operation`,
        );
    });

    after(function () {
        stopForkedMongod(conn, daemonPid);
    });
});
