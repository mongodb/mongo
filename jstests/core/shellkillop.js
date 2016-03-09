baseName = "jstests_shellkillop";

// 'retry' should be set to true in contexts where an exception should cause the test to be retried
// rather than to fail.
retry = false;

function testShellAutokillop() {
    if (true) {  // toggle to disable test
        db[baseName].drop();

        print("shellkillop.js insert data");
        for (i = 0; i < 100000; ++i) {
            db[baseName].insert({i: 1});
        }
        assert.eq(100000, db[baseName].count());

        // mongo --autokillop suppressed the ctrl-c "do you want to kill current operation" message
        // it's just for testing purposes and thus not in the shell help
        var evalStr = "print('SKO subtask started'); db." + baseName +
            ".update( {}, {$set:{i:'abcdefghijkl'}}, false, true ); db." + baseName + ".count();";
        print("shellkillop.js evalStr:" + evalStr);
        spawn = startMongoProgramNoConnect(
            "mongo", "--autokillop", "--port", myPort(), "--eval", evalStr);

        sleep(100);
        retry = true;
        assert(db[baseName].find({i: 'abcdefghijkl'}).count() < 100000,
               "update ran too fast, test won't be valid");
        retry = false;

        stopMongoProgramByPid(spawn);

        sleep(100);

        print("count abcdefghijkl:" + db[baseName].find({i: 'abcdefghijkl'}).count());

        var inprog = db.currentOp().inprog;
        for (i in inprog) {
            if (inprog[i].ns == "test." + baseName)
                throw Error("shellkillop.js op is still running: " + tojson(inprog[i]));
        }

        retry = true;
        assert(db[baseName].find({i: 'abcdefghijkl'}).count() < 100000,
               "update ran too fast, test was not valid");
        retry = false;
    }
}

for (var nTries = 0; nTries < 10 && retry; ++nTries) {
    try {
        testShellAutokillop();
    } catch (e) {
        if (!retry) {
            throw e;
        }
        printjson(e);
        print("retrying...");
    }
}

assert(!retry, "retried too many times");

print("shellkillop.js SUCCESS");
