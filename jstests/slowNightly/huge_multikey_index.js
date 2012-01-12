// https://jira.mongodb.org/browse/SERVER-4534
t = db.huge_multikey_index
t.drop()

function doit() {
    arr = []
    for (var i=0; i< 1000*1000;i++)
        arr.push(i);

    t.insert({a:arr})

    //t.ensureIndex({a:1}, {background:true}) // works!

    t.ensureIndex({a:1}) // boom!
}

// doit(); // SERVER-4534
