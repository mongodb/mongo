load("jstests/libs/parallelTester.js");

/**
 * Implements a kill session test helper
 */
var _kill_sessions_api_module = (function() {
    "use strict";

    var KillSessionsTestHelper = {};

    function isdbgrid(client) {
        var result = assert.commandWorked(client.getDB("admin").runCommand({ismaster: 1}));

        return result.msg === "isdbgrid";
    }

    function Fixture(clientToExecuteVia, clientToKillVia, clientsToVerifyVia) {
        this._clientToExecuteVia = clientToExecuteVia;
        this._clientToExecuteViaIsdbgrid = isdbgrid(clientToExecuteVia);
        this._clientToKillVia = clientToKillVia;
        this._clientToKillViaIsdbgrid = isdbgrid(clientToKillVia);
        this._clientsToVerifyVia = clientsToVerifyVia;
        this._hangingOpId = 10000000;
    }

    Fixture.prototype.loginForExecute = function(cred) {
        this._clientToExecuteVia.getDB("admin").auth(cred, "password");
        this._clientToExecuteViaCredentials = cred;
    };

    Fixture.prototype.loginForKill = function(cred) {
        this._clientToKillVia.getDB("admin").auth(cred, "password");
        this._clientToKillViaCredentials = cred;
    };

    Fixture.prototype.logout = function(db) {
        this._clientToExecuteVia.logout("admin");
        this._clientToKillVia.logout("admin");
    };

    Fixture.prototype.kill = function(db, command) {
        var result = this._clientToKillVia.getDB(db).runCommand(command);
        if (!result.ok) {
            print(tojson(result));
        }
        assert(result.ok);
    };

    Fixture.prototype.assertKillFailed = function(db, command) {
        var result = this._clientToKillVia.getDB(db).runCommand(command);
        if (result.ok) {
            print(tojson(result), tojson(command));
        }
        assert(!result.ok);
    };

    Fixture.prototype.visit = function(cb) {
        for (var i = 0; i < this._clientsToVerifyVia.length; ++i) {
            cb(this._clientsToVerifyVia[i], i);
        }
    };

    function HangingOpHandle(thread, lsid) {
        this._thread = thread;
        this._lsid = lsid;
    }

    HangingOpHandle.prototype.join = function() {
        return this._thread.join();
    };

    HangingOpHandle.prototype.getLsid = function() {
        return this._lsid;
    };

    /**
     * We start hanging ops by running the test command sleep with a special number of secs that's
     * meant to be universal per test run.  In mongos, we multicast it out (to guarantee it's on all
     * of the random accessory nodes).
     */
    Fixture.prototype.startHangingOp = function() {
        var id = this._hangingOpId++;
        // When creating a hanging op, we have to use a background thread (because the command will
        // hang).
        var thread = new Thread(
            function(connstring, credentials, tag, isdbgrid) {
                var client = new Mongo(connstring);
                if (credentials) {
                    client.getDB("admin").auth(credentials, "password");
                }
                var session = client.startSession();
                var db = session.getDatabase("admin");
                try {
                    var cmd = {
                        sleep: 1,
                        lock: "none",
                        secs: tag,
                    };
                    if (isdbgrid) {
                        db.runCommand({
                            multicast: cmd,
                        });
                    } else {
                        db.runCommand(cmd);
                    }
                } catch (e) {
                }
                session.endSession();
                client.close();
            },
            this._clientToExecuteVia.host,
            this._clientToExecuteViaCredentials,
            id,
            this._clientToExecuteViaIsdbgrid);
        thread.start();

        var lsid;
        // We verify that our hanging op is up by looking for it in current op on the required
        // hosts.  We identify particular ops by secs sleeping.
        this.visit(function(client) {
            let admin = client.getDB("admin");
            admin.getMongo().setSecondaryOk();

            assert.soon(function() {
                let inProgressOps = admin.aggregate([{$currentOp: {'allUsers': true}}]);
                while (inProgressOps.hasNext()) {
                    let op = inProgressOps.next();
                    if (op.command && op.command.sleep && op.command.secs == id && op.lsid) {
                        lsid = op.lsid;
                        return true;
                    }
                }

                return false;
            }, "never started sleep with 'secs' " + id, 30000, 1);
        });

        return new HangingOpHandle(thread, lsid);
    };

    /**
     * passes the lsid of each running op into our callback.  Makes it easy to ensure that a session
     * is well and truly dead
     */
    Fixture.prototype.assertNoSessionsInCurrentOp = function(cb) {
        this.visit(function(client) {
            var inprog = client.getDB("admin").currentOp().inprog;
            inprog.forEach(function(op) {
                assert(op.killPending || !op.lsid);
            });
        });
    };

    Fixture.prototype.assertSessionsInCurrentOp = function(checkExist, checkNotExist) {
        this.visit(function(client) {
            var needToFind = checkExist.map(function(handle) {
                return handle.getLsid();
            });
            var inprog = client.getDB("admin").currentOp().inprog;
            inprog.forEach(function(op) {
                if (op.lsid && !op.killPending) {
                    checkNotExist.forEach(function(handle) {
                        assert.neq(bsonWoCompare(handle.getLsid(), op.lsid), 0);
                    });

                    for (var i = 0; i < needToFind.length; ++i) {
                        if (bsonWoCompare(needToFind[i], op.lsid) == 0) {
                            needToFind.splice(i, 1);
                            break;
                        }
                    }
                }
            });

            assert.eq(needToFind.length, 0);
        });
    };

    /**
     * Asserts that there are no sessions in any live cursors.
     */
    Fixture.prototype.assertNoSessionsInCursors = function() {
        this.visit(function(client) {
            var db = client.getDB("admin");
            db.setSecondaryOk();
            assert.soon(() => {
                let cursors = db.aggregate([
                                    {"$currentOp": {"idleCursors": true, "allUsers": true}}
                                ]).toArray();
                return cursors.every(cursor => !cursor.lsid);
            });
        });
    };

    /**
     * Asserts that one subset of sessions is alive in active cursors and that another set is not.
     */
    Fixture.prototype.assertSessionsInCursors = function(checkExist, checkNotExist) {
        this.visit(function(client) {
            var needToFind = checkExist.map(function(handle) {
                return {
                    lsid: handle.getLsid(),
                };
            });

            var db = client.getDB("admin");
            db.setSecondaryOk();
            var cursors = db.aggregate([
                                {"$currentOp": {"idleCursors": true, "allUsers": true}},
                                {"$match": {type: "idleCursor"}}
                            ]).toArray();
            cursors.forEach(function(cursor) {
                if (cursor.lsid) {
                    checkNotExist.forEach(function(handle) {
                        assert.neq(bsonWoCompare({x: handle.getLsid().id}, {x: cursor.lsid.id}), 0);
                    });

                    for (var i = 0; i < needToFind.length; ++i) {
                        if (bsonWoCompare({x: needToFind[i].lsid.id}, {x: cursor.lsid.id}) == 0) {
                            needToFind.splice(i, 1);
                            break;
                        }
                    }
                }
            });
            assert.eq(needToFind.length, 0, cursors);
        });
    };

    Fixture.prototype.assertCursorKillLogMessages = function(cursorHandles) {
        cursorHandles.forEach(
            cursorHandle => cursorHandle.assertCursorKillLogMessages(this._clientsToVerifyVia));
    };

    function CursorHandle(session, cursors) {
        this._session = session;
        this._cursors = cursors;
    }

    CursorHandle.prototype.getLsid = function() {
        return this._session._serverSession.handle.getId();
    };

    CursorHandle.prototype.join = function() {
        this._session.endSession();
    };

    CursorHandle.prototype.assertCursorKillLogMessages = function(hostsToCheck) {
        for (let hostToCheck of hostsToCheck) {
            if (hostToCheck.host in this._cursors) {
                assert(checkLog.checkContainsOnceJsonStringMatch(
                           hostToCheck,
                           20528,
                           'cursorId',
                           this._cursors[hostToCheck.host].exactValueString),
                       "cursor kill was not logged by " + hostToCheck.host);
            }
        }
    };

    /**
     * We start cursors on all nodes in a very artificial way.  In particular, we run currentOp
     * (because it's a data source that's always there) and use an agg pipeline to get enough
     * records that we can't return it in one batch.  In sharding, we use multicast to force open
     * cursors on all nodes at once.
     */
    Fixture.prototype.startCursor = function() {
        var session = this._clientToExecuteVia.startSession();
        var db = session.getDatabase("admin");

        var cmd = {
            aggregate: 1,
            pipeline: [
                {"$currentOp": {"allUsers": false}},
                {"$addFields": {magic: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}},
                {"$project": {magic: 1}},
                {"$unwind": "$magic"}
            ],
            cursor: {batchSize: 1},
            "$readPreference": {
                mode: "primaryPreferred",
            },
            readConcern: {},
            writeConcern: {w: 1},
        };

        var cursors = {};
        var result;
        if (this._clientToExecuteViaIsdbgrid) {
            result = db.runCommand({multicast: cmd});
            for (let host of Object.keys(result.hosts)) {
                cursors[host] = result.hosts[host].data.cursor.id;
            }
        } else {
            result = db.runCommand(cmd);
            cursors[db.getMongo().host] = result.cursor.id;
        }
        if (!result.ok) {
            print(tojson(result));
        }
        assert(result.ok);

        return new CursorHandle(session, cursors);
    };

    /**
     * A suite of tests verifying that various forms of no argument kill work.  Outside of auth
     */
    function makeNoAuthNoArgKill(cmd) {
        var obj = {};
        obj[cmd] = [];
        return [
            // Verify that we can invoke cmd
            function(fixture) {
                fixture.kill("admin", obj);
            },

            // Verify that we can start a session and kill it with cmd
            function(fixture) {
                var handle = fixture.startHangingOp();
                fixture.kill("admin", obj);
                fixture.assertNoSessionsInCurrentOp();
                handle.join();
            },

            // Verify that we can kill two sessions
            function(fixture) {
                var handle1 = fixture.startHangingOp();
                var handle2 = fixture.startHangingOp();
                fixture.kill("admin", obj);
                fixture.assertNoSessionsInCurrentOp();
                handle1.join();
                handle2.join();
            },

            // Verify that we can start a session with a cursor and kill it with cmd
            function(fixture) {
                var handle = fixture.startCursor();
                fixture.assertSessionsInCursors([handle], []);
                fixture.kill("admin", obj);
                fixture.assertNoSessionsInCursors();
                fixture.assertCursorKillLogMessages([handle]);
                handle.join();
            },

            // Verify that we can kill two sessions with cursors
            function(fixture) {
                var handle1 = fixture.startCursor();
                var handle2 = fixture.startCursor();
                fixture.assertSessionsInCursors([handle1, handle2], []);
                fixture.kill("admin", obj);
                fixture.assertNoSessionsInCursors();
                fixture.assertCursorKillLogMessages([handle1, handle2]);
                handle1.join();
                handle2.join();
            },
        ];
    }

    /**
     * Verifies that various argument taking kills work.  Outside of auth
     */
    function makeNoAuthArgKill(cmd, genArg) {
        return [
            // Verify that we can kill two of three sessions, and that the other stays alive
            function(fixture) {
                var handle1 = fixture.startHangingOp();
                var handle2 = fixture.startHangingOp();
                var handle3 = fixture.startHangingOp();

                {
                    var obj = {};
                    obj[cmd] = [handle1.getLsid(), handle2.getLsid()].map(genArg);
                    fixture.kill("admin", obj);
                }
                fixture.assertSessionsInCurrentOp([handle3], [handle1, handle2]);
                handle1.join();
                handle2.join();

                {
                    var obj = {};
                    obj[cmd] = [handle3.getLsid()].map(genArg);
                    fixture.kill("admin", obj);
                }
                fixture.assertNoSessionsInCurrentOp();
                handle3.join();
            },

            // Verify that we can kill two of three sessions, and that the other stays (with
            // cursors)
            function(fixture) {
                var handle1 = fixture.startCursor();
                var handle2 = fixture.startCursor();
                var handle3 = fixture.startCursor();
                fixture.assertSessionsInCursors([handle1, handle2, handle3], []);

                {
                    var obj = {};
                    obj[cmd] = [handle1.getLsid(), handle2.getLsid()].map(genArg);
                    fixture.kill("admin", obj);
                }
                fixture.assertSessionsInCursors([handle3], [handle1, handle2]);
                fixture.assertCursorKillLogMessages([handle1, handle2]);
                handle1.join();
                handle2.join();

                {
                    var obj = {};
                    obj[cmd] = [handle3.getLsid()].map(genArg);
                    fixture.kill("admin", obj);
                }
                fixture.assertNoSessionsInCursors();
                fixture.assertCursorKillLogMessages([handle3]);
                handle3.join();
            },
        ];
    }

    var noAuth = [
        // Verify that we can kill two sessions with the kill all args from killAllSessionsByPattern
        function(fixture) {
            var handle1 = fixture.startHangingOp();
            var handle2 = fixture.startHangingOp();
            fixture.kill("admin", {killAllSessionsByPattern: [{}]});
            fixture.assertNoSessionsInCurrentOp();
            handle1.join();
            handle2.join();
        },
    ];

    // Runs our noarg suite for all commands
    ["killSessions", "killAllSessions", "killAllSessionsByPattern"].forEach(function(cmd) {
        noAuth = noAuth.concat(makeNoAuthNoArgKill(cmd));
    });

    [[
        // Verifies that we can killSessions by lsid
        "killSessions",
        function(x) {
            if (!x.uid) {
                return {
                    id: x.id,
                    uid: computeSHA256Block(""),
                };
            } else {
                return x;
            }
        }
    ],
     [
         // Verifies that we can kill by pattern by lsid
         "killAllSessionsByPattern",
         function(x) {
             if (!x.uid) {
                 return {
                     lsid: {
                         id: x.id,
                         uid: computeSHA256Block(""),
                     }
                 };
             } else {
                 return {lsid: x};
             }
         }
     ]].forEach(function(cmd) {
        noAuth = noAuth.concat(makeNoAuthArgKill.apply({}, cmd));
    });

    KillSessionsTestHelper.runNoAuth = function(
        clientToExecuteVia, clientToKillVia, clientsToVerifyVia) {
        var fixture = new Fixture(clientToExecuteVia, clientToKillVia, clientsToVerifyVia);

        for (var i = 0; i < noAuth.length; ++i) {
            noAuth[i](fixture);
        }
    };

    // Run tests for a command that takes no args in auth
    function makeAuthNoArgKill(cmd, execUserCred, killUserCred) {
        var obj = {};
        obj[cmd] = [];
        return [
            // Verify that we can invoke cmd
            function(fixture) {
                fixture.loginForExecute(execUserCred);
                fixture.loginForKill(killUserCred);
                fixture.kill("admin", obj);
            },

            // Verify that we can start a session and kill it with cmd
            function(fixture) {
                fixture.loginForExecute(execUserCred);
                fixture.loginForKill(killUserCred);
                var handle = fixture.startHangingOp();
                fixture.kill("admin", obj);
                fixture.assertNoSessionsInCurrentOp();
                handle.join();
            },
        ];
    }

    // Run tests for a command that takes args in auth.
    //
    // The genArg argument is a function which returns another function given a username.  It's
    // meant to adapt various lsid's from ops/cursors to various commands.
    function makeAuthArgKill(cmd, execUserCred1, execUserCred2, killUserCred, genArg) {
        return [
            // Run 3 ops, 2 under 1 user, 1 under another.  Then kill, making sure that the 3rd op
            // stays up
            function(fixture) {
                fixture.loginForExecute(execUserCred1);
                var handle1 = fixture.startHangingOp();
                var handle2 = fixture.startHangingOp();

                fixture.logout();
                fixture.loginForExecute(execUserCred2);
                var handle3 = fixture.startHangingOp();

                fixture.loginForKill(killUserCred);

                {
                    var obj = {};
                    obj[cmd] = [handle1.getLsid(), handle2.getLsid()].map(genArg(execUserCred1));
                    fixture.kill("admin", obj);
                }
                fixture.assertSessionsInCurrentOp([handle3], [handle1, handle2]);
                handle1.join();
                handle2.join();

                {
                    var obj = {};
                    obj[cmd] = [handle3.getLsid()].map(genArg(execUserCred2));
                    fixture.kill("admin", obj);
                }
                fixture.assertNoSessionsInCurrentOp();
                handle3.join();
            },

            // Repeat for cursors
            function(fixture) {
                fixture.loginForExecute(execUserCred1);

                var handle1 = fixture.startCursor();
                var handle2 = fixture.startCursor();

                fixture.logout();
                fixture.loginForExecute(execUserCred2);

                var handle3 = fixture.startCursor();
                fixture.assertSessionsInCursors([handle1, handle2, handle3], []);

                fixture.loginForKill(killUserCred);

                {
                    var obj = {};
                    obj[cmd] = [handle1.getLsid(), handle2.getLsid()].map(genArg(execUserCred1));
                    fixture.kill("admin", obj);
                }
                fixture.assertSessionsInCursors([handle3], [handle1, handle2]);
                fixture.assertCursorKillLogMessages([handle1, handle2]);
                handle1.join();
                handle2.join();

                {
                    var obj = {};
                    obj[cmd] = [handle3.getLsid()].map(genArg(execUserCred2));
                    fixture.kill("admin", obj);
                }
                fixture.assertNoSessionsInCursors();
                fixture.assertCursorKillLogMessages([handle3]);
                handle3.join();
            },
        ];
    }

    var auth = [
        // Verify that we can start a session and kill it with the universal pattern
        function(fixture) {
            fixture.loginForExecute("simple");
            fixture.loginForKill("killAny");
            var handle = fixture.startHangingOp();
            fixture.kill("admin", {killAllSessionsByPattern: [{}]});
            fixture.assertNoSessionsInCurrentOp();
            handle.join();
        },
        // Verify that we can impersonate, with that permission
        function(fixture) {
            fixture.loginForExecute("simple");
            fixture.loginForKill("impersonate");
            var handle = fixture.startHangingOp();
            fixture.kill("admin", {killAllSessionsByPattern: [{users: [], roles: []}]});
            fixture.assertNoSessionsInCurrentOp();
            handle.join();
        },
    ];

    // Tests for makeAuthNoArgKill
    [[
        // We can kill our own sessions
        "killSessions",
        "simple",
        "simple",
    ],
     [
         // We can kill all sessions
         "killAllSessions",
         "simple",
         "killAny",
     ],
     [
         // We can kill all sessions by pattern
         "killAllSessionsByPattern",
         "simple",
         "killAny",
     ]].forEach(function(cmd) {
        auth = auth.concat(makeAuthNoArgKill.apply({}, cmd));
    });

    // Tests for makeAuthArgKill
    [[
        // We can kill our own sessions by id (spoofing our own id)
        "killSessions",
        "simple",
        "simple",
        "killAny",
        function() {
            return function(x) {
                if (!x.uid) {
                    return {
                        id: x.id,
                        uid: computeSHA256Block("simple@admin"),
                    };
                } else {
                    return x;
                }
            };
        }
    ],
     [
         // We can kill our own sessions without spoofing
         "killSessions",
         "simple",
         "simple",
         "simple",
         function() {
             return function(x) {
                 return x;
             };
         }
     ],
     [
         // We can kill by pattern by id
         "killAllSessionsByPattern",
         "simple",
         "simple",
         "killAny",
         function() {
             return function(x) {
                 if (!x.uid) {
                     return {
                         lsid: {
                             id: x.id,
                             uid: computeSHA256Block("simple@admin"),
                         }
                     };
                 } else {
                     return {lsid: x};
                 }
             };
         }
     ],
     [
         // We can kill any by user
         "killAllSessions",
         "simple",
         "simple2",
         "killAny",
         function(user) {
             return function(x) {
                 return {db: "admin", user: user};
             };
         }
     ],
     [
         // We can kill any by pattern by user
         "killAllSessionsByPattern",
         "simple",
         "simple2",
         "killAny",
         function(user) {
             return function(x) {
                 return {uid: computeSHA256Block(user + "@admin")};
             };
         }
     ]].forEach(function(cmd) {
        auth = auth.concat(makeAuthArgKill.apply({}, cmd));
    });

    // Ensures that illegal things fail
    function makeAuthArgKillFailure(cmd, execUserCred, killUserCred, genArg) {
        return [
            function(fixture) {
                fixture.loginForExecute(execUserCred);
                fixture.loginForKill(killUserCred);

                var session = fixture._clientToExecuteVia.startSession();

                var obj = {};
                obj[cmd] = [session._serverSession.handle.getId()].map(genArg(execUserCred));
                fixture.assertKillFailed("admin", obj);
                session.endSession();
            },
        ];
    }

    // Tests for makeAuthArgKillFailure
    [[
        // We can't kill another users sessions
        "killSessions",
        "simple",
        "simple2",
        function(user) {
            return function(x) {
                return {
                    id: x.id,
                    uid: computeSHA256Block(user + "@admin"),
                };
            };
        },
    ],
     [
         // We can't impersonate without impersonate
         "killAllSessionsByPattern",
         "simple",
         "killAny",
         function(user) {
             return function(x) {
                 return {
                     users: {},
                     roles: {},
                 };
             };
         },
     ]].forEach(function(cmd) {
        auth = auth.concat(makeAuthArgKillFailure.apply({}, cmd));
    });

    KillSessionsTestHelper.runAuth = function(
        clientToExecuteVia, clientToKillVia, clientsToVerifyVia) {
        var fixture = new Fixture(clientToExecuteVia, clientToKillVia, clientsToVerifyVia);

        for (var i = 0; i < auth.length; ++i) {
            fixture.logout();
            auth[i](fixture);
        }
    };

    KillSessionsTestHelper.initializeAuth = function(client) {
        var admin = client.getDB("admin");
        admin.createUser({user: 'super', pwd: 'password', roles: jsTest.adminUserRoles});
        admin.auth("super", "password");
        admin.createRole({
            role: 'killAnySession',
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ['killAnySession']}]
        });
        admin.createRole({
            role: 'forSimpleTest',
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ['inprog']}]
        });
        admin.createRole({
            role: 'forImpersonate',
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ['impersonate']}]
        });

        admin.createUser({user: 'simple', pwd: 'password', roles: ['forSimpleTest']});
        admin.createUser({user: 'simple2', pwd: 'password', roles: ['forSimpleTest']});
        admin.createUser({user: 'killAny', pwd: 'password', roles: ['killAnySession']});
        admin.createUser(
            {user: 'impersonate', pwd: 'password', roles: ['forImpersonate', 'killAnySession']});
    };

    var module = {};
    module.KillSessionsTestHelper = KillSessionsTestHelper;

    return module;
})();

// Globals
KillSessionsTestHelper = _kill_sessions_api_module.KillSessionsTestHelper;
