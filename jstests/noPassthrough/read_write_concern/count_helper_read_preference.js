// Tests that the read preference set on the connection is used when we call the count helper.
let commandsRan = [];

// Create a new DB object backed by a mock connection.
function MockMongo() {
    this.getMinWireVersion = function getMinWireVersion() {
        return 0;
    };

    this.getMaxWireVersion = function getMaxWireVersion() {
        return 0;
    };
}
MockMongo.prototype = Mongo.prototype;
MockMongo.prototype.runCommand = function (db, cmd, opts) {
    commandsRan.push({db: db, cmd: cmd, opts: opts});
    return {ok: 1, n: 100};
};

const mockMongo = new MockMongo();
var db = new DB(mockMongo, "test");

// Attach a dummy implicit session because the mock connection cannot create sessions.
db._session = new _DummyDriverSession(mockMongo);

assert.eq(commandsRan.length, 0);

// Run a count with no readPref.
db.getMongo().setReadPref(null);
db.foo.count();

// Check that there is no readPref on the command document.
assert.eq(commandsRan.length, 1);
assert.docEq({count: "foo", query: {}}, commandsRan[0].cmd);

commandsRan = [];

// Run with readPref secondary.
db.getMongo().setReadPref("secondary");
db.foo.count();

// Check that we have correctly attached the read preference to the command.
assert.eq(commandsRan.length, 1);
assert.docEq({count: "foo", query: {}, $readPreference: {mode: "secondary"}}, commandsRan[0].cmd);
