let t = db.create_indexes_shell_helper;
t.drop();

let mongo = db.getMongo();

try {
    let commandsRan = [];
    let insertsRan = [];
    let mockMongo = {
        getSecondaryOk: function () {
            return true;
        },
        runCommand: function (db, cmd, opts) {
            commandsRan.push({db: db, cmd: cmd, opts: opts});
            return {ok: 1.0};
        },
        getWriteConcern: function () {
            return null;
        },
        getMinWireVersion: function () {
            return mongo.getMinWireVersion();
        },
        getMaxWireVersion: function () {
            return mongo.getMaxWireVersion();
        },
        isReplicaSetMember: function () {
            return mongo.isReplicaSetMember();
        },
        isMongos: function () {
            return mongo.isMongos();
        },
        isCausalConsistency: function () {
            return false;
        },
        getClusterTime: function () {
            return null;
        },
    };

    db._mongo = mockMongo;
    db._session = new _DummyDriverSession(mockMongo);

    t.createIndexes([{x: 1}]);
    assert.eq(commandsRan.length, 1);
    assert(commandsRan[0].cmd.hasOwnProperty("createIndexes"));
    assert.eq(commandsRan[0].cmd["indexes"][0], {key: {x: 1}, name: "x_1"});

    commandsRan = [];

    t.createIndexes([{y: 1}, {z: -1}]);
    assert.eq(commandsRan.length, 1);
    assert(commandsRan[0].cmd.hasOwnProperty("createIndexes"));
    assert.eq(commandsRan[0].cmd["indexes"][0], {key: {y: 1}, name: "y_1"});
    assert.eq(commandsRan[0].cmd["indexes"][1], {key: {z: -1}, name: "z_-1"});

    commandsRan = [];

    t.createIndex({a: 1});
    assert.eq(commandsRan.length, 1);
    assert(commandsRan[0].cmd.hasOwnProperty("createIndexes"));
    assert.eq(commandsRan[0].cmd["indexes"][0], {key: {a: 1}, name: "a_1"});
} finally {
    db._mongo = mongo;
    db._session = new _DummyDriverSession(mongo);
}
