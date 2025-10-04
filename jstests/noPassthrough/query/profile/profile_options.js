// @tags: [requires_profiling]

import {testGetCmdLineOptsMongod, testGetCmdLineOptsMongos} from "jstests/libs/command_line/test_parsed_options.js";

let baseName = "jstests_core_profile_options";

jsTest.log('Testing "profile" command line option with profiling off');
var expectedResult = {"parsed": {"operationProfiling": {"mode": "off"}}};
testGetCmdLineOptsMongod({profile: "0"}, expectedResult);

jsTest.log('Testing "profile" command line option with profiling slow operations on');
var expectedResult = {"parsed": {"operationProfiling": {"mode": "slowOp"}}};
testGetCmdLineOptsMongod({profile: "1"}, expectedResult);

jsTest.log('Testing "profile" command line option with profiling all on');
var expectedResult = {"parsed": {"operationProfiling": {"mode": "all"}}};
testGetCmdLineOptsMongod({profile: "2"}, expectedResult);

jsTest.log('Testing "operationProfiling.mode" config file option');
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_profiling.json",
        "operationProfiling": {"mode": "all"},
    },
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_profiling.json"}, expectedResult);

jsTest.log('Testing "operationProfiling.filter" config file option with agg expressions');
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_profiling_filter.json",
        "operationProfiling": {"filter": "{$expr: {$lt: [{$rand: {}}, 0.01]}}"},
    },
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_profiling_filter.json"}, expectedResult);
testGetCmdLineOptsMongos({config: "jstests/libs/config_files/set_profiling_filter.json"}, expectedResult);

jsTest.log('Testing "operationProfiling.filter" config with agg expressions that depend on lastLTS feature flag');
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_profiling_filter_ff.json",
        "operationProfiling": {"filter": "{$expr: {$lt: [{$_testFeatureFlagLastLTS: 1}, 0.01]}}"},
    },
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_profiling_filter_ff.json"}, expectedResult);
print(baseName + " succeeded.");
