
function doTest() {
    var baseName = "jstests_dur_journaling_options";

    load('jstests/libs/command_line/test_parsed_options.js');

    jsTest.log("Testing \"dur\" command line option");
    var expectedResult = {"parsed": {"storage": {"journal": {"enabled": true}}}};
    testGetCmdLineOptsMongod({dur: ""}, expectedResult);

    jsTest.log("Testing \"nodur\" command line option");
    expectedResult = {"parsed": {"storage": {"journal": {"enabled": false}}}};
    testGetCmdLineOptsMongod({nodur: ""}, expectedResult);

    jsTest.log("Testing \"journal\" command line option");
    expectedResult = {"parsed": {"storage": {"journal": {"enabled": true}}}};
    testGetCmdLineOptsMongod({journal: ""}, expectedResult);

    jsTest.log("Testing \"nojournal\" command line option");
    expectedResult = {"parsed": {"storage": {"journal": {"enabled": false}}}};
    testGetCmdLineOptsMongod({nojournal: ""}, expectedResult);

    jsTest.log("Testing \"storage.journal.enabled\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/enable_journal.json",
            "storage": {"journal": {"enabled": false}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_journal.json"},
                             expectedResult);

    // Test that we preserve switches explicitly set to false in config files.  See SERVER-13439.
    jsTest.log("Testing explicitly disabled \"journal\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_journal.ini",
            "storage": {"journal": {"enabled": false}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_journal.ini"},
                             expectedResult);

    jsTest.log("Testing explicitly disabled \"nojournal\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_nojournal.ini",
            "storage": {"journal": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_nojournal.ini"},
                             expectedResult);

    jsTest.log("Testing explicitly disabled \"dur\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_dur.ini",
            "storage": {"journal": {"enabled": false}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_dur.ini"}, expectedResult);

    jsTest.log("Testing explicitly disabled \"nodur\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/disable_nodur.ini",
            "storage": {"journal": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_nodur.ini"},
                             expectedResult);

    // Test that switches in old config files with no value have an implicit value of true
    jsTest.log("Testing implicitly enabled \"journal\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/implicitly_enable_journal.ini",
            "storage": {"journal": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/implicitly_enable_journal.ini"},
                             expectedResult);

    jsTest.log("Testing implicitly enabled \"nojournal\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/implicitly_enable_nojournal.ini",
            "storage": {"journal": {"enabled": false}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/implicitly_enable_nojournal.ini"},
                             expectedResult);

    jsTest.log("Testing implicitly enabled \"dur\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/implicitly_enable_dur.ini",
            "storage": {"journal": {"enabled": true}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/implicitly_enable_dur.ini"},
                             expectedResult);

    jsTest.log("Testing implicitly enabled \"nodur\" config file option");
    expectedResult = {
        "parsed": {
            "config": "jstests/libs/config_files/implicitly_enable_nodur.ini",
            "storage": {"journal": {"enabled": false}}
        }
    };
    testGetCmdLineOptsMongod({config: "jstests/libs/config_files/implicitly_enable_nodur.ini"},
                             expectedResult);

    print(baseName + " succeeded.");
}

if (!jsTestOptions().noJournal) {
    doTest();
}
