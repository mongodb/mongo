(function() {
    "use strict";
    if (!db.getMongo().useReadCommands()) {
        var testDB = db.getSiblingDB("blah");
        // test that we can run the 'inprog' pseudocommand on any database.
        assert.commandWorked(testDB.$cmd.sys.inprog.findOne());
    }

})();
