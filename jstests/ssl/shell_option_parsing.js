// Test mongo shell connect strings.
(function() {
    'use strict';

    const SERVER_CERT = "jstests/libs/server.pem";
    const CAFILE = "jstests/libs/ca.pem";

    var opts = {
        sslMode: "allowSSL",
        sslPEMKeyFile: SERVER_CERT,
        sslAllowInvalidCertificates: "",
        sslAllowConnectionsWithoutCertificates: "",
        sslCAFile: CAFILE,
        setParameter: "authenticationMechanisms=MONGODB-X509,SCRAM-SHA-1"
    };

    var rst = new ReplSetTest({name: 'sslSet', nodes: 3, nodeOptions: opts});

    rst.startSet();
    rst.initiate();

    const mongod = rst.getPrimary();
    const host = mongod.host;
    const port = mongod.port;

    const username = "user";
    const usernameNotTest = "userNotTest";
    const usernameX509 = "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=client";

    const password = username;
    const passwordNotTest = usernameNotTest;

    mongod.getDB("test").createUser({user: username, pwd: username, roles: []});
    mongod.getDB("notTest").createUser({user: usernameNotTest, pwd: usernameNotTest, roles: []});
    mongod.getDB("$external").createUser({user: usernameX509, roles: []});

    var i = 0;
    function testConnect(expectPasswordPrompt, expectSuccess, ...args) {
        const command = [
            'mongo',
            '--setShellParameter',
            'newLineAfterPasswordPromptForTest=true',
            '--eval',
            ';',
            '--ssl',
            '--sslAllowInvalidHostnames',
            '--sslCAFile',
            CAFILE,
            ...args
        ];
        print("=========================================> The command (" + (i++) +
              ") I am going to run is: " + command.join(' '));

        clearRawMongoProgramOutput();
        var clientPID = _startMongoProgram({args: command});

        assert.soon(function() {
            const output = rawMongoProgramOutput();
            if (expectPasswordPrompt) {
                if (output.includes("Enter password:")) {
                    stopMongoProgramByPid(clientPID);
                    return true;
                }
                return false;
            }

            let childStatus = checkProgram(clientPID);
            if (!childStatus.alive) {
                if (expectSuccess) {
                    assert.eq(childStatus.exitCode, 0);
                } else {
                    assert.eq(childStatus.exitCode, 1);
                }
                return true;
            }

            return false;
        });
    }

    const testSuccessfulConnect = function(expectPasswordPrompt, ...args) {
        testConnect(expectPasswordPrompt, true, ...args);
    };

    const testFailedConnect = function(expectPasswordPrompt, ...args) {
        testConnect(expectPasswordPrompt, false, ...args);
    };

    testSuccessfulConnect(true, `mongodb://${username}@${host}/test`);
    testSuccessfulConnect(true, `mongodb://${username}@${host}/test`, '--password');

    testSuccessfulConnect(true, `mongodb://${username}@${host}/test`, '--username', username);
    testSuccessfulConnect(
        true, `mongodb://${username}@${host}/test`, '--password', '--username', username);

    testSuccessfulConnect(true,
                          `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                          '--password',
                          '--username',
                          usernameNotTest);

    testSuccessfulConnect(true, `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`);

    testSuccessfulConnect(true,
                          `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                          '--password',
                          '--username',
                          usernameNotTest,
                          '--authenticationDatabase',
                          'notTest');

    testSuccessfulConnect(true,
                          `mongodb://${usernameNotTest}@${host}/test`,
                          '--password',
                          '--username',
                          usernameNotTest,
                          '--authenticationDatabase',
                          'notTest');

    testSuccessfulConnect(
        true, `mongodb://${host}/test?authSource=notTest`, '--username', usernameNotTest);

    testSuccessfulConnect(true, `mongodb://${host}/test`, '--username', username);
    testSuccessfulConnect(true, `mongodb://${host}/test`, '--password', '--username', username);

    testSuccessfulConnect(
        false, `mongodb://${host}/test`, '--password', password, '--username', username);

    testSuccessfulConnect(false, `mongodb://${username}:${password}@${host}/test`);
    testSuccessfulConnect(false, `mongodb://${username}:${password}@${host}/test`, '--password');
    testSuccessfulConnect(
        false, `mongodb://${username}:${password}@${host}/test`, '--password', password);
    testSuccessfulConnect(false, `mongodb://${username}@${host}/test`, '--password', password);

    testSuccessfulConnect(false,
                          `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                          '--username',
                          usernameNotTest,
                          '--password',
                          passwordNotTest,
                          '--authenticationDatabase',
                          'notTest');

    testSuccessfulConnect(false,
                          `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                          '--username',
                          usernameNotTest,
                          '--password',
                          passwordNotTest);

    testSuccessfulConnect(false,
                          `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                          '--password',
                          passwordNotTest);

    testSuccessfulConnect(false,
                          `mongodb://${host}/test?authSource=notTest`,
                          '--username',
                          usernameNotTest,
                          '--password',
                          passwordNotTest);

    // TODO: Enable this set of tests in the future -- needs proper encoding for X509 username in
    // URI
    if (false) {
        testSuccessfulConnect(
            false,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`);
        testSuccessfulConnect(
            false,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
            '--username',
            usernameX509);
        testSuccessfulConnect(false,
                              `mongodb://${usernameX509}@${host}/test?authSource=$external`,
                              '--authenticationMechanism',
                              'MONGODB-X509');

        testSuccessfulConnect(
            false,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
            '--authenticationMechanism',
            'MONGODB-X509');
        testSuccessfulConnect(
            false,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
            '--authenticationMechanism',
            'MONGODB-X509',
            '--username',
            usernameX509);
        testSuccessfulConnect(false,
                              `mongodb://${usernameX509}@${host}/test?authSource=$external`,
                              '--authenticationMechanism',
                              'MONGODB-X509');
    }
    /* */

    testFailedConnect(false,
                      `mongodb://${host}/test?authMechanism=MONGODB-X509&authSource=$external`);
    testFailedConnect(false,
                      `mongodb://${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
                      '--username',
                      usernameX509);

    testFailedConnect(false,
                      `mongodb://${host}/test?authSource=$external`,
                      '--authenticationMechanism',
                      'MONGODB-X509');
    testFailedConnect(false,
                      `mongodb://${host}/test?authSource=$external`,
                      '--username',
                      usernameX509,
                      '--authenticationMechanism',
                      'MONGODB-X509');
    rst.stopSet();
})();
