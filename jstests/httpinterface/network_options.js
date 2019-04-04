var baseName = "jstests_core_network_options";

// Tests for command line option canonicalization.  See SERVER-13379.

load('jstests/libs/command_line/test_parsed_options.js');

// Object Check
jsTest.log("Testing \"objcheck\" command line option");
var expectedResult = {"parsed": {"net": {"wireObjectCheck": true}}};
testGetCmdLineOptsMerizod({objcheck: ""}, expectedResult);

jsTest.log("Testing \"noobjcheck\" command line option");
expectedResult = {
    "parsed": {"net": {"wireObjectCheck": false}}
};
testGetCmdLineOptsMerizod({noobjcheck: ""}, expectedResult);

jsTest.log("Testing \"net.wireObjectCheck\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_objcheck.json",
        "net": {"wireObjectCheck": true}
    }
};
testGetCmdLineOptsMerizod({config: "jstests/libs/config_files/enable_objcheck.json"},
                         expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed": {"net": {}}
};
testGetCmdLineOptsMerizod({}, expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed": {"net": {}}
};
testGetCmdLineOptsMerizod({}, expectedResult);

// Unix Socket
if (!_isWindows()) {
    jsTest.log("Testing \"nounixsocket\" command line option");
    expectedResult = {"parsed": {"net": {"unixDomainSocket": {"enabled": false}}}};
    testGetCmdLineOptsMerizod({nounixsocket: ""}, expectedResult);

    jsTest.log("Testing \"net.wireObjectCheck\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/enable_unixsocket.json",
            "net": {"unixDomainSocket": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMerizod({config: "jstests/libs/config_files/enable_unixsocket.json"},
                             expectedResult);

    jsTest.log("Testing with no explicit network option setting");
    expectedResult = {"parsed": {"net": {}}};
    testGetCmdLineOptsMerizod({}, expectedResult);
}

jsTest.log("Testing explicitly disabled \"objcheck\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_objcheck.ini",
        "net": {"wireObjectCheck": false}
    }
};
testGetCmdLineOptsMerizod({config: "jstests/libs/config_files/disable_objcheck.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"noobjcheck\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_noobjcheck.ini",
        "net": {"wireObjectCheck": true}
    }
};
testGetCmdLineOptsMerizod({config: "jstests/libs/config_files/disable_noobjcheck.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"ipv6\" config file option");
expectedResult = {
    "parsed": {"config": "jstests/libs/config_files/disable_ipv6.ini", "net": {"ipv6": false}}
};
testGetCmdLineOptsMerizod({config: "jstests/libs/config_files/disable_ipv6.ini"}, expectedResult);

if (!_isWindows()) {
    jsTest.log("Testing explicitly disabled \"nounixsocket\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_nounixsocket.ini",
            "net": {"unixDomainSocket": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMerizod({config: "jstests/libs/config_files/disable_nounixsocket.ini"},
                             expectedResult);
}

print(baseName + " succeeded.");
