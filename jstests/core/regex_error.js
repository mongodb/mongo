/**
 * Test that the server errors when given an invalid regex.
 */
(function() {
const coll = db.regex_error;
coll.drop();

// Run some invalid regexes.
assert.commandFailedWithCode(coll.runCommand("find", {filter: {a: {$regex: "[)"}}}), 51091);
assert.commandFailedWithCode(coll.runCommand("find", {filter: {a: {$regex: "ab\0c"}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(
    coll.runCommand("find", {filter: {a: {$regex: "ab", $options: "\0i"}}}), ErrorCodes.BadValue);

// SERVER-58705: Should fail without a memory leak
assert.commandFailedWithCode(coll.runCommand("find", {
    filter: {
        a: {
            $regex:
                "(?JJJ)>?W((?<a>!)||(?<a><aR)|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!)|;|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>!?!)|(?<a>W!| P(?<e>!) C(?<a>!)||(?<a>!aR)|(?<a>!?!)||(?<a>)|(?<a>!6]|P(?<a>!)|(?<aa>!?!);|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR|(?<a>!?!);|(?<a>W!) (?<a>!)||(?<a>)|(?<a>||(?<a><a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>!?!);|(?<a>W(?<a>!) C(?<a>!)||(?<a>!aR)|(?<a>|1|t(?<a>)(?<a>!??<aR)|(?<a>!:aW ) C(?<a>!)||(?<a>!aR)|(?<a>!?!|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR)|(?<a>!(?<a>)(?<a><aR)|(?<a>!:aW ) C(?<a>!)|;|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>!?!);|(?<aa>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>!?!)|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR)|(?<a>!?!)||(?<a>)|(?<a>!2]|P(?<a>!)|C(?<a>!()|(?<aa>!?!);|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR|(?<a>!?!);|(?<a>W!) __P(?<a>!-||(?<a>)|(?<a>||(?<a><a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>!?!);|(?<a>W(?<a>!) C(?<a>!)||(?<a>!aR)|(?<a>|1|t(?<a>)(?<a>!??<a!) __P(?<a>!|(?<a>)|(?<a>!3]|P(?<a>!)|C(?<a>!)||(?<a>!aR)|(?<a>!?!);|(?<a>W!| P(?<a>!) C(?<a>!)|;|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>! m);|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR)|(?<a>!?!)||(?<a>)|(?<a>!6]|P(?<a>!)|C(?<a>!()(?<aa>!?!);|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!?!);|(?<a>W!) !)||(?<a>)|(?<a>!6]|P(?<a>!)|(?<aa>!?!);|(?<a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR|(?<a>!?!);|(?<a>W!) (?<a>!)||(?<a>)|(?<a>||(?<a><a>W!| P(?<a>!) C(?<a>!)||(?<a>!aR(?<a>!aR)|(?<a>!?!);|(?<a>W(?<a>!) C(?<a>!)||(?<a>!aR)|(?<a>|1|t(?<a>)(?<a>!??<__P(?<a>!||(?<a>)|(?<a>||(?<a>!?<"
        }
    }
}),
                             51091);
})();
