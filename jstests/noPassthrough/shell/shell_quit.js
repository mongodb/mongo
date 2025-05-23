let checkShell = function(retCode) {
    let args = [
        "mongo",
        "--nodb",
        "--eval",
        "quit(" + retCode + ");",
    ];

    let actualRetCode = _runMongoProgram.apply(null, args);
    assert.eq(retCode, actualRetCode);
};

checkShell(0);
checkShell(5);
