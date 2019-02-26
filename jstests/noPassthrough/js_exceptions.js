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
    tests.forEach(function(t) {
        let code = tojson(recurser);
        [1, 2, 10].forEach(function(depth) {
            clearRawMongoProgramOutput();
            assert.throws(startParallelShell(
                code + ";\nrecurser(0," + depth + "," + tojson(t.callback) + ");", false, true));
            let output = rawMongoProgramOutput();
            let lines = output.split("\n");
            assert(/MongoDB shell version/.test(lines.shift()));
            assert(/^\s*$/.test(lines.pop()));
            assert(/exiting with code/.test(lines.pop()));

            let m = new RegExp("\\\[js\\\] " + t.match + "$");
            assert(m.test(lines.shift()));

            if (t.stack == true) {
                assert.eq(lines.length,
                          depth + 2);  // plus one for the shell and one for the callback
                lines.forEach(function(l) {
                    assert(/\@\(shell eval\):\d+:\d+/.test(l));
                });
                lines.pop();
                lines.shift();
                lines.forEach(function(l) {
                    assert(/recurser\@/.test(l));
                });
            } else if (t.stack == false) {
                assert.eq(lines.length, 0);
            } else if (t.stack == undefined) {
                assert.eq(lines.length, 1);
                assert(/undefined/.test(lines.pop()));
            }
        });
    });
})();
