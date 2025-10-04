let checkShell = function (retCode) {
    let args = ["mongo", "--nodb", "--eval", "quit(" + retCode + ");"];

    let actualRetCode = _runMongoProgram(...args);
    assert.eq(retCode, actualRetCode);
};

checkShell(0);
checkShell(5);
