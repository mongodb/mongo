// Tests --networkMessageCompressors options.

let runTest = function (optionValue, expected) {
    jsTest.log('Testing with --networkMessageCompressors="' + optionValue + '" expecting: ' + expected);
    let mongo = MongoRunner.runMongod({networkMessageCompressors: optionValue});
    assert.commandWorked(mongo.adminCommand({hello: 1}));
    clearRawMongoProgramOutput();
    assert.eq(
        runMongoProgram(
            "mongo",
            "--eval",
            "tostrictjson(db.hello());",
            "--port",
            mongo.port,
            "--networkMessageCompressors=snappy",
        ),
        0,
    );

    let output = rawMongoProgramOutput(".*")
        .split("\n")
        .map(function (str) {
            str = str.replace(/^sh[0-9]+\| /, "");
            if (!/^{.*isWritablePrimary/.test(str)) {
                return "";
            }
            return str;
        })
        .join("\n")
        .trim();

    output = JSON.parse(output);

    assert.eq(output.compression, expected);
    MongoRunner.stopMongod(mongo);
};

assert.throws(() => MongoRunner.runMongod({networkMessageCompressors: "snappy,disabled"}));

runTest("snappy", ["snappy"]);
runTest("disabled", undefined);
