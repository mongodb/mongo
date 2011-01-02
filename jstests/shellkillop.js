baseName = "jstests_shellkillop";

if (_isWindows()) {
    print("shellkillop.js not testing on windows, as functionality is missing there");
    print("shellkillop.js see http://jira.mongodb.org/browse/SERVER-1451");
}
else {

    db[baseName].drop();

    print("shellkillop.js insert data");
    for (i = 0; i < 100000; ++i) {
        db[baseName].insert({ i: 1 });
    }
    assert.eq(100000, db[baseName].count());

    // mongo --autokillop suppressed the ctrl-c "do you want to kill current operation" message
    // it's just for testing purposes and thus not in the shell help
    var evalStr = "print('SKO subtask started'); db." + baseName + ".update( {}, {$set:{i:'abcdefghijkl'}}, false, true ); db." + baseName + ".count();";
    print("shellkillop.js evalStr:" + evalStr);
    spawn = startMongoProgramNoConnect("mongo", "--autokillop", "--port", myPort(), "--eval", evalStr);

    sleep(100);
    assert(db[baseName].find({ i: 'abcdefghijkl' }).count() < 100000, "update ran too fast, test won't be valid");

    stopMongoProgramByPid(spawn);

    sleep(200);

    print("count abcdefghijkl:" + db[baseName].find({ i: 'abcdefghijkl' }).count());

    assert.soon(
    function f() {
        var inprog = db.currentOp().inprog;
        for (i in inprog) {
            if (inprog[i].ns == "test." + baseName)
                print("shellkillop.js op is still running, waiting");
            //printjson(inprog);
            return false;
        }
        return true;
    },
    "shellkillop.js FAIL op is still running"
    );

    print("shellkillop.js SUCCESS");

}
