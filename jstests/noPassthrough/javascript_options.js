(function() {
"use strict";

load('jstests/libs/command_line/test_parsed_options.js');

let expectedResult = {"parsed": {"security": {"javascriptEnabled": false}}};
testGetCmdLineOptsMongod({noscripting: ""}, expectedResult);
testGetCmdLineOptsMongos({noscripting: ""}, expectedResult);

expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_noscripting.ini",
        "security": {"javascriptEnabled": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_noscripting.ini"},
                         expectedResult);
testGetCmdLineOptsMongos({config: "jstests/libs/config_files/disable_noscripting.ini"},
                         expectedResult);

expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_scripting.json",
        "security": {"javascriptEnabled": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_scripting.json"},
                         expectedResult);
testGetCmdLineOptsMongos({config: "jstests/libs/config_files/enable_scripting.json"},
                         expectedResult);
}());
