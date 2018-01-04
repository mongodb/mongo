(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let mongoUri = "mongodb://" + rst.nodes.map((node) => node.host).join(",") + "/test";
    let conn = rst.nodes[0];

    // There are three ways to enable retryable writes in the mongo shell.
    // 1. (cmdline flag) start mongo shell with --retryWrites
    // 2. (uri param) connect to a uri like mongodb://.../test?retryWrites=true
    // 3. (session option) in mongo shell create a new session with {retryWrite: true}

    function runShellScript(uri, cmdArgs, insertShouldHaveTxnNumber, shellFn) {
        // This function is stringified and called immediately in the mongo --eval.
        function testWrapper(insertShouldHaveTxnNumber, shellFn) {
            const mongoRunCommandOriginal = Mongo.prototype.runCommand;
            let insertFound = false;
            Mongo.prototype.runCommand = function runCommandSpy(dbName, cmdObj, options) {
                let cmdObjSeen = cmdObj;
                let cmdName = Object.keys(cmdObjSeen)[0];

                if (cmdName === "query" || cmdName === "$query") {
                    cmdObjSeen = cmdObjSeen[cmdName];
                    cmdName = Object.keys(cmdObj)[0];
                }

                if (cmdName === "insert") {
                    insertFound = true;
                    if (insertShouldHaveTxnNumber) {
                        assert(cmdObjSeen.hasOwnProperty("txnNumber"),
                               "insert sent without expected txnNumber");
                    } else {
                        assert(!cmdObjSeen.hasOwnProperty("txnNumber"),
                               "insert sent with txnNumber unexpectedly");
                    }
                }
                return mongoRunCommandOriginal.apply(this, arguments);
            };

            shellFn();
            assert(insertFound, "test did not run insert command");
        }

        // Construct the string to be passed to eval.
        let script = "(" + testWrapper.toString() + ")(";
        script += insertShouldHaveTxnNumber + ",";
        script += shellFn.toString();
        script += ")";

        let args = ["./mongo", uri, "--eval", script].concat(cmdArgs);
        let exitCode = runMongoProgram(...args);
        assert.eq(exitCode, 0, `shell script "${shellFn.name}" exited with ${exitCode}`);
    }

    // Tests --retryWrites command line parameter.
    runShellScript(mongoUri, ["--retryWrites"], true, function flagWorks() {
        assert(db.getSession().getOptions().shouldRetryWrites(), "retryWrites should be true");
        assert.writeOK(db.coll.insert({}), "cannot insert");
    });

    // The uri param should override --retryWrites.
    runShellScript(
        mongoUri + "?retryWrites=false", ["--retryWrites"], false, function flagOverridenByUri() {
            assert(!db.getSession().getOptions().shouldRetryWrites(),
                   "retryWrites should be false");
            assert.writeOK(db.coll.insert({}), "cannot insert");
        });

    // Even if initial connection has retryWrites=false in uri, new connections should not be
    // overriden.
    runShellScript(mongoUri + "?retryWrites=false",
                   ["--retryWrites"],
                   true,
                   function flagNotOverridenByNewConn() {
                       let connUri = db.getMongo().host;  // does not have ?retryWrites=false.
                       let sess = new Mongo(connUri).startSession();
                       assert(sess.getOptions().shouldRetryWrites(), "retryWrites should be true");
                       assert.writeOK(sess.getDatabase("test").coll.insert({}), "cannot insert");
                   });

    // Unless that uri also specifies retryWrites.
    runShellScript(mongoUri + "?retryWrites=false",
                   ["--retryWrites"],
                   false,
                   function flagOverridenInNewConn() {
                       let connUri = "mongodb://" + db.getMongo().host + "/test?retryWrites=false";
                       let sess = new Mongo(connUri).startSession();
                       assert(!sess.getOptions().shouldRetryWrites(),
                              "retryWrites should be false");
                       assert.writeOK(sess.getDatabase("test").coll.insert({}), "cannot insert");
                   });

    // Session options should override --retryWrites as well.
    runShellScript(mongoUri, ["--retryWrites"], false, function flagOverridenByOpts() {
        let connUri = "mongodb://" + db.getMongo().host + "/test";
        let sess = new Mongo(connUri).startSession({retryWrites: false});
        assert(!sess.getOptions().shouldRetryWrites(), "retryWrites should be false");
        assert.writeOK(sess.getDatabase("test").coll.insert({}), "cannot insert");
    });

    // Test uri retryWrites parameter.
    runShellScript(mongoUri + "?retryWrites=true", [], true, function uriTrueWorks() {
        assert(db.getSession().getOptions().shouldRetryWrites(), "retryWrites should be true");
        assert.writeOK(db.coll.insert({}), "cannot insert");
    });

    // Test that uri retryWrites=false works.
    runShellScript(mongoUri + "?retryWrites=false", [], false, function uriFalseWorks() {
        assert(!db.getSession().getOptions().shouldRetryWrites(), "retryWrites should be false");
        assert.writeOK(db.coll.insert({}), "cannot insert");
    });

    // Test SessionOptions retryWrites option.
    runShellScript(mongoUri, [], true, function sessOptTrueWorks() {
        let connUri = "mongodb://" + db.getMongo().host + "/test";
        let sess = new Mongo(connUri).startSession({retryWrites: true});
        assert(sess.getOptions().shouldRetryWrites(), "retryWrites should be true");
        assert.writeOK(sess.getDatabase("test").coll.insert({}), "cannot insert");
    });

    // Test that SessionOptions retryWrites:false works.
    runShellScript(mongoUri, [], false, function sessOptFalseWorks() {
        let connUri = "mongodb://" + db.getMongo().host + "/test";
        let sess = new Mongo(connUri).startSession({retryWrites: false});
        assert(!sess.getOptions().shouldRetryWrites(), "retryWrites should be false");
        assert.writeOK(sess.getDatabase("test").coll.insert({}), "cannot insert");
    });

    // Test that session option overrides uri option.
    runShellScript(mongoUri + "?retryWrites=true", [], false, function sessOptOverridesUri() {
        let sess = db.getMongo().startSession({retryWrites: false});
        assert(!sess.getOptions().shouldRetryWrites(), "retryWrites should be false");
        assert.writeOK(sess.getDatabase("test").coll.insert({}), "cannot insert");
    });

    rst.stopSet();
}());
