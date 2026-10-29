// Tests for command line option canonicalization.  See SERVER-13379.
import {testGetCmdLineOptsMongod} from "jstests/libs/command_line/test_parsed_options.js";

let baseName = "jstests_core_network_options";

jsTest.log("Testing with no explicit network option setting");
let expectedResult = {
    "parsed": {"net": {}},
};
testGetCmdLineOptsMongod({}, expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed": {"net": {}},
};
testGetCmdLineOptsMongod({}, expectedResult);

// Unix Socket
if (!_isWindows()) {
    jsTest.log('Testing "nounixsocket" command line option');
    expectedResult = {"parsed": {"net": {"unixDomainSocket": {"enabled": false}}}};
    testGetCmdLineOptsMongod({nounixsocket: ""}, expectedResult);

    jsTest.log('Testing "net.unixDomainSocket" config file option');
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/enable_unixsocket.json",
            "net": {"unixDomainSocket": {"enabled": true}},
        },
    };
    testGetCmdLineOptsMongod(
        {config: "jstests/libs/config_files/enable_unixsocket.json"},
        expectedResult,
    );

    jsTest.log("Testing with no explicit network option setting");
    expectedResult = {"parsed": {"net": {}}};
    testGetCmdLineOptsMongod({}, expectedResult);
}

jsTest.log('Testing explicitly disabled "ipv6" config file option');
expectedResult = {
    "parsed": {"config": "jstests/libs/config_files/disable_ipv6.ini", "net": {"ipv6": false}},
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_ipv6.ini"}, expectedResult);

if (!_isWindows()) {
    jsTest.log('Testing explicitly disabled "nounixsocket" config file option');
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_nounixsocket.ini",
            "net": {"unixDomainSocket": {"enabled": true}},
        },
    };
    testGetCmdLineOptsMongod(
        {config: "jstests/libs/config_files/disable_nounixsocket.ini"},
        expectedResult,
    );
}

print(baseName + " succeeded.");
