let t = db.push2;
t.drop();

t.save({_id: 1, a: []});

// 3.5 MB chunk: 4 pushes succeed (~14 MB array), the 5th exceeds the 16 MB BSON
// document limit and is rejected.
let s = "x".repeat(3.5 * 1024 * 1024);

let gotError = null;

for (let x = 0; x < 5; x++) {
    print(x + " pushes");
    let res = t.update({}, {$push: {a: s}});
    gotError = res.hasWriteError();
    if (gotError) break;
}

assert(gotError, "should have gotten error");

t.drop();
