(function() {
    'use strict';
    var checkShell = function(retCode) {
        var args = [
            "bongo",
            "--nodb",
            "--eval",
            "quit(" + retCode + ");",
        ];

        var actualRetCode = _runBongoProgram.apply(null, args);
        assert.eq(retCode, actualRetCode);
    };

    checkShell(0);
    checkShell(5);
})();
