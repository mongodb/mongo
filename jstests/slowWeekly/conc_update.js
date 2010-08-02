db = db.getSisterDB("concurrency")
db.dropDatabase();

NRECORDS=5*1024*1024 // this needs to be relatively big so that
                      // the update() will take a while, but it could
                      // probably be smaller.

print("loading "+NRECORDS+" documents (progress msg every 1024*1024 documents)")
for (i=0; i<(NRECORDS); i++) {
    db.conc.insert({x:i})
    if ((i%(1024*1024))==0)
	print("loaded " + i/(1024*1024) + " mibi-records")
}

print("making an index (this will take a while)")
db.conc.ensureIndex({x:1})

var c1=db.conc.count({x:{$lt:NRECORDS}})
// this is just a flag that the child will toggle when it's done.
db.concflag.update({}, {inprog:true}, true)

updater=startParallelShell("db=db.getSisterDB('concurrency');\
			   db.conc.update({}, {$inc:{x: "+NRECORDS+"}}, false, true);\
			   e=db.getLastError();\
			   print('update error: '+ e);\
			   db.concflag.update({},{inprog:false});\
			   assert.eq(e, null, \"update failed\");");

querycount=0;
decrements=0;
misses=0
while (1) {
    if (db.concflag.findOne().inprog) {
	c2=db.conc.count({x:{$lt:NRECORDS}})
	e=db.getLastError()
 	print(c2)
	print(e)
	assert.eq(e, null, "some count() failed")
	querycount++;
	if (c2<c1)
	    decrements++;
	else
	    misses++;
	c1 = c2;
    } else
	break;
    sleep(10);
}
print(querycount + " queries, " + decrements + " decrements, " + misses + " misses");

updater() // wait()
