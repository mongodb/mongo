var baseName = "jstests_core_network_options";

load('jstests/libs/command_line/test_parsed_options.js');

// Object Check
jsTest.log("Testing \"objcheck\" command line option");
var expectedResult = {
    "parsed" : {
        "net" : {
            "wireObjectCheck" : true
        }
    }
};
testGetCmdLineOptsMongod({ objcheck : "" }, expectedResult);

jsTest.log("Testing \"noobjcheck\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "wireObjectCheck" : false
        }
    }
};
testGetCmdLineOptsMongod({ noobjcheck : "" }, expectedResult);

jsTest.log("Testing \"net.wireObjectCheck\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_objcheck.json",
        "net" : {
            "wireObjectCheck" : true
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_objcheck.json" },
                         expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed" : {
        "net" : { }
    }
};
testGetCmdLineOptsMongod({}, expectedResult);



// HTTP Interface
jsTest.log("Testing \"httpinterface\" command line option");
var expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOptsMongod({ httpinterface : "" }, expectedResult);

jsTest.log("Testing \"nohttpinterface\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "enabled" : false
            }
        }
    }
};
testGetCmdLineOptsMongod({ nohttpinterface : "" }, expectedResult);

jsTest.log("Testing implicit enabling of http interface with \"jsonp\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "JSONPEnabled" : true,
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOptsMongod({ jsonp : "" }, expectedResult);

jsTest.log("Testing implicit enabling of http interface with \"rest\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "RESTInterfaceEnabled" : true,
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOptsMongod({ rest : "" }, expectedResult);

jsTest.log("Testing \"net.http.enabled\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_httpinterface.json",
        "net" : {
            "http" : {
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_httpinterface.json" },
                         expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed" : {
        "net" : { }
    }
};
testGetCmdLineOptsMongod({}, expectedResult);



// Unix Socket
if (!_isWindows()) {
    jsTest.log("Testing \"nounixsocket\" command line option");
    expectedResult = {
        "parsed" : {
            "net" : {
                "unixDomainSocket" : {
                    "enabled" : false
                }
            }
        }
    };
    testGetCmdLineOptsMongod({ nounixsocket : "" }, expectedResult);

    jsTest.log("Testing \"net.wireObjectCheck\" config file option");
    expectedResult = {
        "parsed" : {
            "config" : "jstests/libs/config_files/enable_unixsocket.json",
            "net" : {
                "unixDomainSocket" : {
                    "enabled" : true
                }
            }
        }
    };
    testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_unixsocket.json" },
                       expectedResult);

    jsTest.log("Testing with no explicit network option setting");
    expectedResult = {
        "parsed" : {
            "net" : { }
        }
    };
    testGetCmdLineOptsMongod({}, expectedResult);
}

print(baseName + " succeeded.");
