// $substr returns an empty string if the position argument is out of bounds.  SERVER-6186

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insertOne({}));

function substr(string, pos, n) {
    return t.aggregate({$project: {a: {$substr: [string, pos, n]}}}).toArray()[0].a;
}

function expectedSubstr(string, pos, n) {
    if (n < 0) {
        // A negative value is interpreted as a large unsigned int, expected to exceed the length
        // of the string.  Passing the string length is functionally equivalent.
        n = string.length;
    }
    return string.substring(pos, pos + n);
}

function assertSubstr(string, pos, n) {
    assert.eq(expectedSubstr(string, pos, n), substr(string, pos, n));
}

function checkVariousSubstrings(string) {
    for (let pos = 0; pos < 5; ++pos) {
        for (let n = -2; n < 7; ++n) {
            assertSubstr(string, pos, n);
        }
    }
}

checkVariousSubstrings("abc");
checkVariousSubstrings("");
