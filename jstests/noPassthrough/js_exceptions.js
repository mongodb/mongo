/**
 * Validate that exceptions are reported correctly
 *
 */
let tests = [
    {
        callback: function() {
            UUID("asdf");
        },
        match: "Error: Invalid UUID string: asdf",
        stack: true,
    },
    {
        callback: function() {
            throw {};
        },
        match: "uncaught exception: \\\[object Object\\\]",
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
            // eslint-disable-next-line
            foo.bar();
        },
        match: "uncaught exception: ReferenceError: foo is not defined",
        stack: true,
    },
    {
        callback: function() {
            throw function() {};
        },
        match: "function\\\(\\\) {}",
        stack: undefined,
    },
    {
        callback: function() {
            try {
                UUID("asdf");
            } catch (e) {
                throw (e.constructor());
            }
        },
        match: "uncaught exception: Error",
        stack: true,
    },
    {
        callback: function() {
            try {
                UUID("asdf");
            } catch (e) {
                throw (e.prototype);
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
        let output = rawMongoProgramOutput(".*");
        assert.includes(output, "MongoDB shell version");
        assert.includes(output, "exiting with code");
        assertMatch(new RegExp(t.match), output);

        if (t.stack == true) {
            assert.eq(output.match(/\@\(shell eval\):\d+:\d+/g).length,
                      depth + 2);  // plus one for the shell and one for the callback
            assert.eq(output.match(/recurser\@/g).length, depth);
        } else if (t.stack == false) {
            assert(!output.includes("shell eval"));
            assert(!output.includes("recurser"));
        } else if (t.stack == undefined) {
            assert.includes(output, "undefined");
        }
    });
});
