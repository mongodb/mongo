var st = new ShardingTest( "auth1", 3, 0, 3, {rs: true, extraOptions : {keyFile : "jstests/libs/key1"}, chunksize : 1});

var mongos = st.s0;
var adminDB = mongos.getDB( 'admin' );
var configDB = mongos.getDB( 'config' );

var aDB = mongos.getDB( 'A' );
var bDB = mongos.getDB( 'B' );

var createShardedCollection = function() {
    adminDB.runCommand({"enableSharding" : aDB.getName()});

    var coll = aDB.coll;
    adminDB.runCommand({"shardCollection" : coll.getFullName(), "key" : {x : 1}});

    populateCollection(coll);

    // make sure there are chunks on both shards
    printjson(configDB.chunks.find().toArray());
    st.awaitBalance( coll.getName(), coll.getDB().getName() , 60 * 5 * 1000 /* 5 minutes */ );
};

var populateCollection = function(collection) {
    for (i=0; i<100000; i++) {
        collection.insert({x:i, y: i, foo:"bar", date: new Date(), r:Math.random()});
    }
    collection.getDB().getLastError();
};

var getROUser = function(x) {
    getUser(x, "RO");
};

var getRWUser = function(x) {
    getUser(x, "RW");
};

var getUser = function(x, type) {
    var id = type+x;

    x.addUser("user"+id, "pwd"+id, type == "RO", 3 /* replicatedTo */);

    print("added user"+id+" to "+x);

    auth(x, type);
};

var auth = function(x, type) {
    var id = type+x;
    assert.eq(1, x.auth("user"+id, "pwd"+id));
}

var doReads = function(x) {
    var cursor = x.coll.find({x:{$gt:1000}}).sort({_id:-1});
    var prev = null;
    i=0;
    while (cursor.hasNext()) {
        var doc = cursor.next();
        assert(prev == null || prev > doc._id, "prev: "+prev+" doc: "+tojson(doc));
        prev = doc._id;
        i++;
    }

    print("i is "+i);

    // read
    assert(x.coll.count() >= 100000, "count is: "+x.coll.count());

    // none
    assert.eq(1, x.runCommand({"ping" : 1}).ok);
};

var testRW = function(x) {
    // reads and writes
    x.coll.update({y : {$mod : [20, 2]}}, {$set : {date : new Date()}}, false, true);
    var result = x.runCommand({getLastError:1});
    printjson(result);
    // TODO: SERVER-6568
    if (x != "admin") {
        assert.eq(5000, result.n);
    }

    doReads(x);

    // commands
    var now = new Date();
    x.coll.findAndModify({query:{x : 10000}, update:{$set : {date : now}}});
    var doc = x.coll.findOne({x:10000});
    assert.eq(doc.date, now, "doc: "+tojson(doc)+" now: "+now);
};

var testRO = function(x) {
    // reads and writes
    print("1");
    x.test.insert({x:true});
    var result = x.runCommand({"getlasterror" : 1});
    printjson(result);
    assert.eq(15845 /* unauthorized */, result.code);
    print("3");
    doReads(x);
    print("4");

    // write
    assert.eq(0, x.runCommand({"reIndex" : "test"}).ok);
    print("6");
};

var profilingOn = function() {
    auth(aDB, "RW");
    aDB.setProfilingLevel(2);
    aDB.logout();
};

var profilingOff = function() {
    auth(aDB, "RW");
    aDB.setProfilingLevel(0);
    aDB.logout();
};

var part1 = function() {
    getROUser(aDB);

    print("Make sure they can RW from any db.");
    print("Test on read cmds, write cmds, non-locking commands.");
    testRW(aDB);
    populateCollection(adminDB.coll);
    testRW(adminDB);

    print("Open new dbs/collections.");
    populateCollection(bDB.coll);
    testRW(bDB);

    print("logout");
    aDB.logout();
}

var part2 = function() {
    var cDB = mongos.getDB("C");
    getROUser(cDB);

    print("Make sure they can RW from any db.");
    print("Test on read cmds, write cmds, non-locking commands.");
    testRW(aDB);
    testRW(adminDB);

    cDB.logout();
}

var part3 = function() {
    // create a RW user first, because it'll be a PITA if we don't later
    getRWUser(adminDB);

    getROUser(adminDB);
    adminDB.logout();

    auth(adminDB, "RO");

    testRO(aDB);
    testRO(adminDB);

    adminDB.logout();
};

var part4 = function() {
    auth(adminDB, "RW");
    populateCollection(configDB.coll);
    getROUser(configDB);
    print("got RO config user");
    adminDB.logout();

    print("getting config");
    testRO(configDB);

    configDB.logout();
};

var part5 = function() {
    auth(adminDB, "RW");
    getRWUser(aDB);
    getROUser(bDB);
    adminDB.logout();

    testRW(aDB);
    testRO(bDB);
};

var part6 = function() {
    profilingOn();

    auth(aDB, "RO");
    testRO(aDB);
    aDB.logout();

    profilingOff();

    print("Turn it on while a read-only user user is logged in and try reading from an old coll/new coll, running commands");
    var aDB2 = st.s1.getDB("A");

    auth(aDB2, "RO");
    testRO(aDB2);
    profilingOn();
    testRO(aDB2);
    aDB2.logout();
    profilingOff();
}

var part7 = function() {
    var otherConn = st.s1;
    var adminDB = otherConn.getDB("admin");
    var aDB = otherConn.getDB("A");
    var bDB = otherConn.getDB("B");

    auth(adminDB, "RW");
    getRWUser(aDB);
    getROUser(bDB);
    adminDB.logout();

    testRW(aDB);
    testRO(bDB);
};

print("set up collections before creating users");
createShardedCollection();

print("Create read-only user, userROA, on A (with no user in the admin db)");
part1();

print("Create read-only user, userROC, on C (with no user in the admin db)");
part2();

print("Create read-only admin user (userROadmin).")
part3();

print("Do the same as #3 with config db RO user (userROconfig)");
part4();

print("If we log in with a RW user on A and then a RO user on B, make sure we can RW on A and can't RW on B");
part5();

print("Test RO user with system profiling on (not sure how broken this is, still).");
part6();

print("From another mongos");
part7();

print("stopping");
st.stop();

