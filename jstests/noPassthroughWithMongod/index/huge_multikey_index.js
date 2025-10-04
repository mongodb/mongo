// https://jira.mongodb.org/browse/SERVER-4534
// Building an index in the foreground on a field with a large array and few documents in
// the collection used to open too many files and crash the server.
let t = db.huge_multikey_index;
t.drop();

function doit() {
    let arr = [];
    for (let i = 0; i < 1000 * 1000; i++) arr.push(i);

    t.insert({a: arr});

    // t.createIndex({a:1}, {background:true}) // always worked

    t.createIndex({a: 1}); // used to fail server with out of fds error
}

doit();
