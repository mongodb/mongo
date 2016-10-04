// Tests that the read preference set on the connection is used when we call the count helper.
(function() {
    "use strict";

    var commandsRan = [];

    // Create a new DB object backed by a mock connection.
    function MockMongo() {
    }
    MockMongo.prototype = Mongo.prototype;
    MockMongo.prototype.runCommand = function(db, cmd, opts) {
        commandsRan.push({db: db, cmd: cmd, opts: opts});
        return {ok: 1, n: 100};
    };
    var db = new DB(new MockMongo(), "test");

    assert.eq(commandsRan.length, 0);

    // Run a count with no readPref.
    db.getMongo().setReadPref(null);
    db.foo.count();

    // Check that there is no readPref on the command document.
    assert.eq(commandsRan.length, 1);
    assert.docEq(commandsRan[0].cmd, {count: "foo", fields: {}, query: {}});

    commandsRan = [];

    // Run with readPref secondary.
    db.getMongo().setReadPref("secondary");
    db.foo.count();

    // Check that we have wrapped the command and attached the read preference.
    assert.eq(commandsRan.length, 1);
    assert.docEq(
        commandsRan[0].cmd,
        {query: {count: "foo", fields: {}, query: {}}, $readPreference: {mode: "secondary"}});

})();
