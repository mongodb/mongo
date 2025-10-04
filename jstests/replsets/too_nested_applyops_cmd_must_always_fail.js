/**
 * Ensures that applyOps command with too many nested applyOps instances fails validation
 * in case of presense of an empty oplog entry among other oplog entries. Previously validation
 * routine was returning before validation in a few cases of super user requirement and thus
 * allowing a su to run a potentially malformed command. Running this test against code versions
 * without the fix will fail for all above cases(empty object, create and renameCollection)
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primaryDB = rst.getPrimary().getDB("admin");
let nestedCmd = {op: "n", ns: ""};
for (let i = 0; i < 10; i++) {
    nestedCmd = {op: "c", ns: "admin.$cmd", o: {applyOps: [nestedCmd]}};
}
// 10 recursion steps are still fine
assert.commandWorked(primaryDB.adminCommand({"applyOps": [nestedCmd]}));

// adding 10 more to break the max recursion depth rule
for (let i = 0; i < 10; i++) {
    nestedCmd = {op: "c", ns: "admin.$cmd", o: {applyOps: [nestedCmd]}};
}

assert.commandFailedWithCode(primaryDB.adminCommand({"applyOps": [nestedCmd]}), ErrorCodes.FailedToParse);

// Adding an empty entry to existing command. Empty entry requires a superuser, but we expect
// parsing failure now too.
assert.commandFailedWithCode(
    primaryDB.adminCommand({"applyOps": [nestedCmd, {op: "c", ns: "admin.$cmd", o: {"applyOps": []}}]}),
    ErrorCodes.FailedToParse,
);

// Prepending a "create" command to our nested command, without the fix we would have succeeded here
// because of early return
assert.commandFailedWithCode(
    primaryDB.adminCommand({
        "applyOps": [
            {
                op: "c",
                ns: "admin.$cmd",
                o: {
                    "applyOps": [
                        {
                            "op": "c",
                            "ns": "admin.$cmd",
                            "o": {
                                "create": "x",
                            },
                        },
                        nestedCmd,
                    ],
                },
            },
        ],
    }),
    ErrorCodes.FailedToParse,
);
// Prepending a "renameCollection" command to our nested command, without the fix we would have
// succeeded here because of early return

assert.commandFailedWithCode(
    primaryDB.adminCommand({
        "applyOps": [
            {
                op: "c",
                ns: "admin.$cmd",
                o: {
                    "applyOps": [{"op": "c", "ns": "admin.$cmd", "o": {renameCollection: "", to: "test.b"}}, nestedCmd],
                },
            },
        ],
    }),
    ErrorCodes.FailedToParse,
);
rst.stopSet();
