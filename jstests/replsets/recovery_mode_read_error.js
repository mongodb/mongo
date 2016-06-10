
/*
    This test checks to make sure nodes that are in RECOVERING mode
    cannot accept reads and throw the correct errors.
        - use replSetMaintenance to send secondary into recovering mode
        - run a find() on secondary to make sure it throws the "node is
         recovering" error message

*/

(function() {
    "use strict";
    var replTest = new ReplSetTest({name: 'recovery_mode_read_error', nodes: 2});
    var conns = replTest.startSet();

    replTest.initiate();

    // Make sure we have a master
    var primary = replTest.getPrimary();
    primary.getDB("bar").foo.insert({foo: 3});

    var secondary = replTest.getSecondary();

    print("secondary going into maintenance mode (recovery mode)");
    assert.commandWorked(secondary.adminCommand({replSetMaintenance: 1}));

    var recv = secondary.getDB("bar").runCommand({find: "foo"});
    assert.commandFailed(recv);
    assert.eq(recv.errmsg, "node is recovering");

})();
