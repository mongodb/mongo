var db1 = (new Mongo("merizodb://localhost:20000")).getDB("merizoreplay");

db1.test.insert({dummy:true});
for(var i =0; i < 10; i ++ ){
  db1.test.find().limit(10).toArray()
}

db1= (new Mongo("merizodb://localhost:20000")).getDB("merizoreplay");

db1.test.insert({dummy:true});
for(var i =0; i < 10; i ++ ){
  db1.test.find().limit(10).toArray()
}
