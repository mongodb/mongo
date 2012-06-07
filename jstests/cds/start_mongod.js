
m = startMongod( "--auth", "--port" , "30001" , "--dbpath", "/data/db/123", "--nohttpinterface", "--nojournal" , "--smallfiles" );
db=m.getDB("admin");
db.addUser("sa","sa");
db.auth("sa","sa");
db=m.getDB("test1");
db.addUser("lockmind","123456");
db.cds.dbmaxconn.save({ "_id" : "cds", "value" : 2 });
db.cds.maxfilenum.save({"_id":"cds","value":2});
db.cds.maxcpucost.save({"_id":"cds","cpu_cost":2,"time_priod":10});
db.cds.whiteip.save({"value":"*","ip":"0.0.0.0/32"});


db=m.getDB("test");
db.addUser("lockmind","123456");
db.cds.dbmaxconn.save({ "_id" : "cds", "value" : 2 }); 
db.cds.maxfilenum.save({"_id":"cds","value":2});
db.cds.maxcpucost.save({"_id":"cds","cpu_cost":2,"time_priod":10});
db.cds.whiteip.save({"value":"*","ip":"0.0.0.0/32"});

sleep(10000000);
