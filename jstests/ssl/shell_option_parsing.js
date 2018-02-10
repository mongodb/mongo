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
    function testConnect(noPasswordPrompt, ...args) {
        const command = [
            'mongo',
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
        var clientPID = _startMongoProgram.apply(null, command);
        sleep(30000);

        if (checkProgram(clientPID).alive) {
            stopMongoProgramByPid(clientPID);
        }

        assert.eq(!noPasswordPrompt, rawMongoProgramOutput().includes("Enter password:"));
    }

    testConnect(false, `mongodb://${username}@${host}/test`);
    testConnect(false, `mongodb://${username}@${host}/test`, '--password');

    testConnect(false, `mongodb://${username}@${host}/test`, '--username', username);
    testConnect(false, `mongodb://${username}@${host}/test`, '--password', '--username', username);

    testConnect(false,
                `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--password',
                '--username',
                usernameNotTest);

    testConnect(false, `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`);

    testConnect(false,
                `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--password',
                '--username',
                usernameNotTest,
                '--authenticationDatabase',
                'notTest');

    testConnect(false,
                `mongodb://${usernameNotTest}@${host}/test`,
                '--password',
                '--username',
                usernameNotTest,
                '--authenticationDatabase',
                'notTest');

    testConnect(false, `mongodb://${host}/test?authSource=notTest`, '--username', usernameNotTest);

    testConnect(false, `mongodb://${host}/test`, '--username', username);
    testConnect(false, `mongodb://${host}/test`, '--password', '--username', username);

    testConnect(true, `mongodb://${host}/test`, '--password', password, '--username', username);

    testConnect(true, `mongodb://${username}:${password}@${host}/test`);
    testConnect(true, `mongodb://${username}:${password}@${host}/test`, '--password');
    testConnect(true, `mongodb://${username}:${password}@${host}/test`, '--password', password);
    testConnect(true, `mongodb://${username}@${host}/test`, '--password', password);

    testConnect(true,
                `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--username',
                usernameNotTest,
                '--password',
                passwordNotTest,
                '--authenticationDatabase',
                'notTest');

    testConnect(true,
                `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--username',
                usernameNotTest,
                '--password',
                passwordNotTest);

    testConnect(true,
                `mongodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--password',
                passwordNotTest);

    testConnect(true,
                `mongodb://${host}/test?authSource=notTest`,
                '--username',
                usernameNotTest,
                '--password',
                passwordNotTest);

    // TODO: Enable this set of tests in the future -- needs proper encoding for X509 username in
    // URI
    if (false) {
        testConnect(
            true,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`);
        testConnect(
            true,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
            '--username',
            usernameX509);
        testConnect(true,
                    `mongodb://${usernameX509}@${host}/test?authSource=$external`,
                    '--authenticationMechanism',
                    'MONGODB-X509');

        testConnect(
            true,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
            '--authenticationMechanism',
            'MONGODB-X509');
        testConnect(
            true,
            `mongodb://${usernameX509}@${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
            '--authenticationMechanism',
            'MONGODB-X509',
            '--username',
            usernameX509);
        testConnect(true,
                    `mongodb://${usernameX509}@${host}/test?authSource=$external`,
                    '--authenticationMechanism',
                    'MONGODB-X509');
    }
    /* */

    testConnect(true, `mongodb://${host}/test?authMechanism=MONGODB-X509&authSource=$external`);
    testConnect(true,
                `mongodb://${host}/test?authMechanism=MONGODB-X509&authSource=$external`,
                '--username',
                usernameX509);

    testConnect(true,
                `mongodb://${host}/test?authSource=$external`,
                '--authenticationMechanism',
                'MONGODB-X509');
    testConnect(true,
                `mongodb://${host}/test?authSource=$external`,
                '--username',
                usernameX509,
                '--authenticationMechanism',
                'MONGODB-X509');
    rst.stopSet();
})();
