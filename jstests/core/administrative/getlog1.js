// @tags: [
//   # The test runs commands that are not allowed with security token: getLog.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns
// ]

// to run:
//   ./mongo jstests/<this-file>

let contains = function (arr, obj) {
    let i = arr.length;
    while (i--) {
        if (arr[i] === obj) {
            return true;
        }
    }
    return false;
};

let resp = db.adminCommand({getLog: "*"});
assert(resp.ok == 1, "error executing getLog command");
assert(resp.names, "no names field");
assert(resp.names.length > 0, "names array is empty");
assert(contains(resp.names, "global"), "missing global category");
assert(!contains(resp.names, "butty"), "missing butty category");

resp = db.adminCommand({getLog: "global"});
assert(resp.ok == 1, "error executing getLog command");
assert(resp.log, "no log field");
assert(resp.log.length > 0, "no log lines");

// getLog value must be a string
assert.commandFailed(db.adminCommand({getLog: 21}));
