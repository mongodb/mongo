path = MongoRunner.dataDir + '/indexbg_dur';

var m = MongoRunner.runMongod({journal: "", smallfiles: "", journalOptions: 24});
t = m.getDB('test').test;
t.save({x: 1});
t.createIndex({x: 1}, {background: true});
t.count();
