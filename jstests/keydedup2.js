t = db.keydedup2;
t.drop();

padding = new Array(5000).toString();
for( i = 0; i < 14; ++i ) {
    t.insert({_id:i,p:padding});
}

t.ensureIndex({a:1,b:1},{sparse:true});

s900 = new Array(901).toString();

for( i = 10; i < 18; ++i ) {
    t.save({a:i,b:s900});
}

for( i = 0; i < 7; ++i ) {
    a = i % 2 ? 50 : NumberLong(50);
    t.update({_id:i},{a:a,b:s900});
    t.update({_id:14-i-1},{a:a,b:s900});
}

function checkSorted( i ) {
    ret = t.find().sort({a:1,b:1}).toArray();
    for( i = 1; i < ret.length; ++i ) {
        assert.lte( ret[i-1].a, ret[i].a );
    }
}
checkSorted();

for( i = 10; i < 15; ++i ) {
    t.remove({a:i});
    assert( !db.getLastError() );
}

checkSorted();
