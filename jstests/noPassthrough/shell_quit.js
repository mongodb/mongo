(function() {
    'use strict';
    var checkShell = function(retCode) {
        var args = [
            "mongo",
            "--nodb",
            "--eval",
            "quit(" + retCode + ");",
        ];

        var actualRetCode = _runMongoProgram.apply(null, args);
        assert.eq(retCode, actualRetCode);
    };

    checkShell(0);
    checkShell(5);
})();
