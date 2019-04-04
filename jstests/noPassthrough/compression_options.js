// Tests --networkMessageCompressors options.

(function() {
    'use strict';

    var runTest = function(optionValue, expected) {
        jsTest.log("Testing with --networkMessageCompressors=\"" + optionValue + "\" expecting: " +
                   expected);
        var merizo = MerizoRunner.runMerizod({networkMessageCompressors: optionValue});
        assert.commandWorked(merizo.adminCommand({isMaster: 1}));
        clearRawMerizoProgramOutput();
        assert.eq(runMerizoProgram("merizo",
                                  "--eval",
                                  "tostrictjson(db.isMaster());",
                                  "--port",
                                  merizo.port,
                                  "--networkMessageCompressors=snappy"),
                  0);

        var output = rawMerizoProgramOutput()
                         .split("\n")
                         .map(function(str) {
                             str = str.replace(/^sh[0-9]+\| /, "");
                             if (!/^{/.test(str)) {
                                 return "";
                             }
                             return str;
                         })
                         .join("\n")
                         .trim();

        output = JSON.parse(output);

        assert.eq(output.compression, expected);
        MerizoRunner.stopMerizod(merizo);
    };

    assert.isnull(MerizoRunner.runMerizod({networkMessageCompressors: "snappy,disabled"}));

    runTest("snappy", ["snappy"]);
    runTest("disabled", undefined);

}());
