(function() {
let t = db.push2;
t.drop();

t.save({_id: 1, a: []});

let s = new Array(700000).toString();

let gotError = null;

for (let x = 0; x < 100; x++) {
    print(x + " pushes");
    var res = t.update({}, {$push: {a: s}});
    gotError = res.hasWriteError();
    if (gotError)
        break;
}

assert(gotError, "should have gotten error");

t.drop();
})();
