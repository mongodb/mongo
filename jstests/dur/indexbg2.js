path = MongoRunner.dataDir + '/indexbg2_dur';

var m = MongoRunner.runMongod({journal: "", smallfiles: ""});

t = m.getDB('test').test;
t.createIndex({a: 1});
t.createIndex({b: 1});
t.createIndex({x: 1}, {background: true});
for (var i = 0; i < 1000; ++i) {
    t.insert({_id: i, a: 'abcd', b: 'bcde', x: 'four score and seven years ago'});
    t.remove({_id: i});
}
sleep(1000);
for (var i = 1000; i < 2000; ++i) {
    t.insert({_id: i, a: 'abcd', b: 'bcde', x: 'four score and seven years ago'});
    t.remove({_id: i});
}
assert.writeOK(t.insert({_id: 2000, a: 'abcd', b: 'bcde', x: 'four score and seven years ago'}));
