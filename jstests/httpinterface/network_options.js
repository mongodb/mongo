var baseName = "jstests_core_network_options";

// Tests for command line option canonicalization.  See SERVER-13379.

load('jstests/libs/command_line/test_parsed_options.js');

// Object Check
jsTest.log("Testing \"objcheck\" command line option");
var expectedResult = {"parsed": {"net": {"wireObjectCheck": true}}};
testGetCmdLineOptsMongod({objcheck: ""}, expectedResult);

jsTest.log("Testing \"noobjcheck\" command line option");
expectedResult = {
    "parsed": {"net": {"wireObjectCheck": false}}
};
testGetCmdLineOptsMongod({noobjcheck: ""}, expectedResult);

jsTest.log("Testing \"net.wireObjectCheck\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_objcheck.json",
        "net": {"wireObjectCheck": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_objcheck.json"},
                         expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed": {"net": {}}
};
testGetCmdLineOptsMongod({}, expectedResult);

// HTTP Interface
jsTest.log("Testing \"httpinterface\" command line option");
var expectedResult = {"parsed": {"net": {"http": {"enabled": true}}}};
testGetCmdLineOptsMongod({httpinterface: ""}, expectedResult);

jsTest.log("Testing \"nohttpinterface\" command line option");
expectedResult = {
    "parsed": {"net": {"http": {"enabled": false}}}
};
testGetCmdLineOptsMongod({nohttpinterface: ""}, expectedResult);

jsTest.log("Testing implicit enabling of http interface with \"jsonp\" command line option");
expectedResult = {
    "parsed": {"net": {"http": {"JSONPEnabled": true, "enabled": true}}}
};
testGetCmdLineOptsMongod({jsonp: ""}, expectedResult);

jsTest.log("Testing implicit enabling of http interface with \"rest\" command line option");
expectedResult = {
    "parsed": {"net": {"http": {"RESTInterfaceEnabled": true, "enabled": true}}}
};
testGetCmdLineOptsMongod({rest: ""}, expectedResult);

jsTest.log("Testing \"net.http.enabled\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_httpinterface.json",
        "net": {"http": {"enabled": true}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_httpinterface.json"},
                         expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed": {"net": {}}
};
testGetCmdLineOptsMongod({}, expectedResult);

// Unix Socket
if (!_isWindows()) {
    jsTest.log("Testing \"nounixsocket\" command line option");
    expectedResult = {"parsed": {"net": {"unixDomainSocket": {"enabled": false}}}};
    testGetCmdLineOptsMongod({nounixsocket: ""}, expectedResult);

    jsTest.log("Testing \"net.wireObjectCheck\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/enable_unixsocket.json",
            "net": {"unixDomainSocket": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_unixsocket.json"},
                             expectedResult);

    jsTest.log("Testing with no explicit network option setting");
    expectedResult = {"parsed": {"net": {}}};
    testGetCmdLineOptsMongod({}, expectedResult);
}

// Test that we preserve switches explicitly set to false in config files.  See SERVER-13439.
jsTest.log("Testing explicitly disabling \"net.http.RESTInterfaceEnabled\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_rest_interface.json",
        "net": {"http": {"RESTInterfaceEnabled": false}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_rest_interface.json"},
                         expectedResult);

jsTest.log("Testing explicitly disabling \"net.http.JSONPEnabled\" config file option on mongoD");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_jsonp.json",
        "net": {"http": {"JSONPEnabled": false}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_jsonp.json"}, expectedResult);

// jsonp on mongos is legacy and not supported in json/yaml config files since this interface is not
// well defined.  See SERVER-11707 for an example.
jsTest.log("Testing explicitly disabling \"jsonp\" config file option on mongoS");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_jsonp.ini",
        "net": {"http": {"JSONPEnabled": false}}
    }
};
testGetCmdLineOptsMongos({config: "jstests/libs/config_files/disable_jsonp.ini"}, expectedResult);

jsTest.log("Testing explicitly disabled \"objcheck\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_objcheck.ini",
        "net": {"wireObjectCheck": false}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_objcheck.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"noobjcheck\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_noobjcheck.ini",
        "net": {"wireObjectCheck": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_noobjcheck.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"httpinterface\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_httpinterface.ini",
        "net": {"http": {"enabled": false}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_httpinterface.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"nohttpinterface\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_nohttpinterface.ini",
        "net": {"http": {"enabled": true}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_nohttpinterface.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"ipv6\" config file option");
expectedResult = {
    "parsed": {"config": "jstests/libs/config_files/disable_ipv6.ini", "net": {"ipv6": false}}
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_ipv6.ini"}, expectedResult);

if (!_isWindows()) {
    jsTest.log("Testing explicitly disabled \"nounixsocket\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_nounixsocket.ini",
            "net": {"unixDomainSocket": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_nounixsocket.ini"},
                             expectedResult);
}

print(baseName + " succeeded.");
