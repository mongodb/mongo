/**
 * Validate that exceptions are reported correctly
 *
 */
(function() {
    'use strict';
    let tests = [
        {
          callback: function() {
              UUID("asdf");
          },
          match: "Error: Invalid UUID string: asdf :",
          stack: true,
        },
        {
          callback: function() {
              throw {};
          },
          match: "uncaught exception: \\\[object Object\\\] :",
          stack: undefined,
        },
        {
          callback: function() {
              throw "asdf";
          },
          match: "uncaught exception: asdf",
          stack: false,
        },
        {
          callback: function() {
              throw 1;
          },
          match: "uncaught exception: 1",
          stack: false,
        },
        {
          callback: function() {
              foo.bar();
          },
          match: "uncaught exception: ReferenceError: foo is not defined :",
          stack: true,
        },
        {
          callback: function() {
              throw function() {};
          },
          match: "function\\\(\\\) {} :",
          stack: undefined,
        },
        {
          callback: function() {
              try {
                  UUID("asdf");
              } catch (e) {
                  throw(e.constructor());
              }
          },
          match: "uncaught exception: Error :",
          stack: true,
        },
        {
          callback: function() {
              try {
                  UUID("asdf");
              } catch (e) {
                  throw(e.prototype);
              }
          },
          match: "uncaught exception: undefined",
          stack: false,
        },
    ];
    function recurser(depth, limit, callback) {
        if (++depth >= limit) {
            callback();
        } else {
            recurser(depth, limit, callback);
        }
    }
    function assertMatch(m, l) {
        assert(m.test(l), m + " didn't match \"" + l + "\"");
    }
    tests.forEach(function(t) {
        let code = tojson(recurser);
        [1, 2, 10].forEach(function(depth) {
            clearRawMongoProgramOutput();
            assert.throws(startParallelShell(
                code + ";\nrecurser(0," + depth + "," + tojson(t.callback) + ");", false, true));
            let output = rawMongoProgramOutput();
            let lines = output.split(/\s*\n/);
            let matchShellExp = false;
            while (lines.length > 0 & matchShellExp !== true) {
                let line = lines.shift();
                if (line.match(/MongoDB shell version/)) {
                    matchShellExp = true;
                }
            }
            assert(matchShellExp);
            assertMatch(/^\s*$/, lines.pop());
            assertMatch(/exiting with code/, lines.pop());
            assertMatch(new RegExp("\\\[js\\\] " + t.match + "$"), lines.shift());

            if (t.stack == true) {
                assert.eq(lines.length,
                          depth + 2);  // plus one for the shell and one for the callback
                lines.forEach(function(l) {
                    assertMatch(/\@\(shell eval\):\d+:\d+/, l);
                });
                lines.pop();
                lines.shift();
                lines.forEach(function(l) {
                    assertMatch(/recurser\@/, l);
                });
            } else if (t.stack == false) {
                assert.eq(lines.length, 0);
            } else if (t.stack == undefined) {
                assert.eq(lines.length, 1);
                assertMatch(/undefined/, lines.pop());
            }
        });
    });
})();
