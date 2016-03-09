
t = db.push2;
t.drop();

t.save({_id: 1, a: []});

s = new Array(700000).toString();

gotError = null;

for (x = 0; x < 100; x++) {
    print(x + " pushes");
    var res = t.update({}, {$push: {a: s}});
    gotError = res.hasWriteError();
    if (gotError)
        break;
}

assert(gotError, "should have gotten error");

t.drop();
