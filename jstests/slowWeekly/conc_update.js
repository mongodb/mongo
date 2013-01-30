load( "jstests/libs/slow_weekly_util.js" )
test = new SlowWeeklyMongod( "conc_update" )
db = test.getDB("concurrency")
db.dropDatabase();

NRECORDS=3*1024*1024 // this needs to be relatively big so that
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

updater=startParallelShell("db=db.getSisterDB('concurrency');\
                           db.concflag.insert( {inprog:true} );\
                           sleep(20);\
			   db.conc.update({}, {$inc:{x: "+NRECORDS+"}}, false, true);\
			   e=db.getLastError();\
			   print('update error: '+ e);\
			   db.concflag.update({},{inprog:false});\
			   assert.eq(e, null, 'update failed');");

assert.soon( function(){ var x = db.concflag.findOne(); return x && x.inprog; } , 
             "wait for fork" , 30000 , 1 );

querycount=0;
decrements=0;
misses=0

assert.soon( 
    function(){
	c2=db.conc.count({x:{$lt:NRECORDS}})
 	print(c2)
	querycount++;
	if (c2<c1)
	    decrements++;
	else
	    misses++;
	c1 = c2;        
        return ! db.concflag.findOne().inprog;
    } , 
    "update never finished" , 2 * 60 * 60 * 1000 , 10 );

print(querycount + " queries, " + decrements + " decrements, " + misses + " misses");

assert.eq( NRECORDS , db.conc.count() , "AT END 1" )

updater() // wait()

test.stop();
