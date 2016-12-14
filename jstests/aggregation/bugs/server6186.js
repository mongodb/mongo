// $substrBytes returns an empty string if the position argument is out of bounds.  SERVER-6186
// SERVER-25173: $substrBytes does not accept negative values as input. Behavior with negative
// values is checked in pipeline/expression_test.cpp.

t = db.jstests_aggregation_server6186;
t.drop();

t.save({});

function substr(string, pos, n) {
    return t.aggregate({$project: {a: {$substrBytes: [string, pos, n]}}}).toArray()[0].a;
}

function expectedSubstr(string, pos, n) {
    return string.substring(pos, pos + n);
}

function assertSubstr(string, pos, n) {
    assert.eq(expectedSubstr(string, pos, n), substr(string, pos, n));
}

function checkVariousSubstrings(string) {
    for (pos = 0; pos < 5; ++pos) {
        for (n = 0; n < 7; ++n) {
            assertSubstr(string, pos, n);
        }
    }
}

checkVariousSubstrings("abc");
checkVariousSubstrings("");
