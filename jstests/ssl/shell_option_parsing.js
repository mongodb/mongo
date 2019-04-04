// Test merizo shell connect strings.
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
        setParameter: "authenticationMechanisms=MERIZODB-X509,SCRAM-SHA-1"
    };

    var rst = new ReplSetTest({name: 'sslSet', nodes: 3, nodeOptions: opts});

    rst.startSet();
    rst.initiate();

    const merizod = rst.getPrimary();
    const host = merizod.host;
    const port = merizod.port;

    const username = "user";
    const usernameNotTest = "userNotTest";
    const usernameX509 = "C=US,ST=New York,L=New York City,O=MerizoDB,OU=KernelUser,CN=client";

    const password = username;
    const passwordNotTest = usernameNotTest;

    merizod.getDB("test").createUser({user: username, pwd: username, roles: []});
    merizod.getDB("notTest").createUser({user: usernameNotTest, pwd: usernameNotTest, roles: []});
    merizod.getDB("$external").createUser({user: usernameX509, roles: []});

    var i = 0;
    function testConnect(noPasswordPrompt, ...args) {
        const command = [
            'merizo',
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

        clearRawMerizoProgramOutput();
        var clientPID = _startMerizoProgram.apply(null, command);
        sleep(30000);

        if (checkProgram(clientPID).alive) {
            stopMerizoProgramByPid(clientPID);
        }

        assert.eq(!noPasswordPrompt, rawMerizoProgramOutput().includes("Enter password:"));
    }

    testConnect(false, `merizodb://${username}@${host}/test`);
    testConnect(false, `merizodb://${username}@${host}/test`, '--password');

    testConnect(false, `merizodb://${username}@${host}/test`, '--username', username);
    testConnect(false, `merizodb://${username}@${host}/test`, '--password', '--username', username);

    testConnect(false,
                `merizodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--password',
                '--username',
                usernameNotTest);

    testConnect(false, `merizodb://${usernameNotTest}@${host}/test?authSource=notTest`);

    testConnect(false,
                `merizodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--password',
                '--username',
                usernameNotTest,
                '--authenticationDatabase',
                'notTest');

    testConnect(false,
                `merizodb://${usernameNotTest}@${host}/test`,
                '--password',
                '--username',
                usernameNotTest,
                '--authenticationDatabase',
                'notTest');

    testConnect(false, `merizodb://${host}/test?authSource=notTest`, '--username', usernameNotTest);

    testConnect(false, `merizodb://${host}/test`, '--username', username);
    testConnect(false, `merizodb://${host}/test`, '--password', '--username', username);

    testConnect(true, `merizodb://${host}/test`, '--password', password, '--username', username);

    testConnect(true, `merizodb://${username}:${password}@${host}/test`);
    testConnect(true, `merizodb://${username}:${password}@${host}/test`, '--password');
    testConnect(true, `merizodb://${username}:${password}@${host}/test`, '--password', password);
    testConnect(true, `merizodb://${username}@${host}/test`, '--password', password);

    testConnect(true,
                `merizodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--username',
                usernameNotTest,
                '--password',
                passwordNotTest,
                '--authenticationDatabase',
                'notTest');

    testConnect(true,
                `merizodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--username',
                usernameNotTest,
                '--password',
                passwordNotTest);

    testConnect(true,
                `merizodb://${usernameNotTest}@${host}/test?authSource=notTest`,
                '--password',
                passwordNotTest);

    testConnect(true,
                `merizodb://${host}/test?authSource=notTest`,
                '--username',
                usernameNotTest,
                '--password',
                passwordNotTest);

    // TODO: Enable this set of tests in the future -- needs proper encoding for X509 username in
    // URI
    if (false) {
        testConnect(
            true,
            `merizodb://${usernameX509}@${host}/test?authMechanism=MERIZODB-X509&authSource=$external`);
        testConnect(
            true,
            `merizodb://${usernameX509}@${host}/test?authMechanism=MERIZODB-X509&authSource=$external`,
            '--username',
            usernameX509);
        testConnect(true,
                    `merizodb://${usernameX509}@${host}/test?authSource=$external`,
                    '--authenticationMechanism',
                    'MERIZODB-X509');

        testConnect(
            true,
            `merizodb://${usernameX509}@${host}/test?authMechanism=MERIZODB-X509&authSource=$external`,
            '--authenticationMechanism',
            'MERIZODB-X509');
        testConnect(
            true,
            `merizodb://${usernameX509}@${host}/test?authMechanism=MERIZODB-X509&authSource=$external`,
            '--authenticationMechanism',
            'MERIZODB-X509',
            '--username',
            usernameX509);
        testConnect(true,
                    `merizodb://${usernameX509}@${host}/test?authSource=$external`,
                    '--authenticationMechanism',
                    'MERIZODB-X509');
    }
    /* */

    testConnect(true, `merizodb://${host}/test?authMechanism=MERIZODB-X509&authSource=$external`);
    testConnect(true,
                `merizodb://${host}/test?authMechanism=MERIZODB-X509&authSource=$external`,
                '--username',
                usernameX509);

    testConnect(true,
                `merizodb://${host}/test?authSource=$external`,
                '--authenticationMechanism',
                'MERIZODB-X509');
    testConnect(true,
                `merizodb://${host}/test?authSource=$external`,
                '--username',
                usernameX509,
                '--authenticationMechanism',
                'MERIZODB-X509');
    rst.stopSet();
})();
