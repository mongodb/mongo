// Test that we validate nToReturn when upconverting pseudoCommands.
(function() {
    "use strict";
    if (!db.getMongo().useReadCommands()) {
        var inprog = db.$cmd.sys.inprog;
        // nToReturn must be 1 or -1.
        assert.doesNotThrow(function() {
            inprog.find().limit(-1).next();
        });
        assert.doesNotThrow(function() {
            inprog.find().limit(1).next();
        });
        assert.throws(function() {
            inprog.find().limit(0).next();
        });
        assert.throws(function() {
            inprog.find().limit(-2).next();
        });
        assert.throws(function() {
            inprog.find().limit(99).next();
        });
    }
})();
