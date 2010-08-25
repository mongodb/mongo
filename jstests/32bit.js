// 32bit.js dm

var forceSeedToBe = .8;

if (forceSeedToBe)
    print("\n32bit.js WARNING FORCING A SPECIFIC SEED - TEST WILL RUN DURING DAY");

function f() {
    seed = forceSeedToBe || Math.random();
    
    pass = 1;

    var mydb = db.getSisterDB( "test_32bit" );
    mydb.dropDatabase();

    while( 1 ) {
        if( pass >= 2 ) 
	    break;
        print("32bit.js PASS #" + pass);
        pass++;
        
        t = mydb.colltest_32bit;

        print("seed=" + seed);
        
        t.insert({x:1});
        t.ensureIndex({a:1});
        t.ensureIndex({b:1}, true);
        t.ensureIndex({x:1});
        if( Math.random() < 0.3 )
	    t.ensureIndex({c:1});
        t.ensureIndex({d:1});
        t.ensureIndex({e:1});
        t.ensureIndex({f:1});
        
        big = 'a                          b';
        big = big + big;
        k = big;
        big = big + big;
        big = big + big;
        big = big + big;
        
        a = 0;
        c = 'kkk';
        var start = new Date();
        while( 1 ) { 
	    b = Math.random(seed);
	    d = c + -a;
            f = Math.random(seed) + a;
            a++;
	    cc = big;
            if( Math.random(seed) < .1 ) 
		cc = null;
	    t.insert({a:a,b:b,c:cc,d:d,f:f});
	    if( Math.random(seed) < 0.01 ) { 

	        if( mydb.getLastError() ) {
		    /* presumably we have mmap error on 32 bit. try a few more manipulations attempting to break things */		
		    t.insert({a:33,b:44,c:55,d:66,f:66});
		    t.insert({a:33,b:44000,c:55,d:66});
		    t.insert({a:33,b:440000,c:55});
		    t.insert({a:33,b:4400000});
		    t.update({a:20},{'$set':{c:'abc'}});
		    t.update({a:21},{'$set':{c:'aadsfbc'}});
		    t.update({a:22},{'$set':{c:'c'}});
		    t.update({a:23},{'$set':{b:cc}});
		    t.remove({a:22});
		    break;
	        }
	        
	        t.remove({a:a});
	        t.remove({b:Math.random(seed)});
	        t.insert({e:1});
	        t.insert({f:'aaaaaaaaaa'});
	        
                if( Math.random() < 0.00001 ) { print("remove cc"); t.remove({c:cc}); }
                if( Math.random() < 0.0001 ) { print("update cc"); t.update({c:cc},{'$set':{c:1}},false,true); }
                if( Math.random() < 0.00001 ) { print("remove e"); t.remove({e:1}); }
	    }
	    if (a == 10000 ) {
		var delta_ms = (new Date())-start;
		// 2MM / 10000 = 200.  1000ms/sec.
		var eta_secs = delta_ms * (200 / 1000);
		print("eta_secs:" + eta_secs);
		print(delta_ms);
		if( eta_secs > 1800 ) {
		    print("machine is slow, stopping early. a:" + a);
		    mydb.dropDatabase();
		    return;
		}
	    }
	    if( a % 100000 == 0 ) {
	        print(a);
	        // on 64 bit we won't error out, so artificially stop.  on 32 bit we will hit mmap limit ~1.6MM but may 
	        // vary by a factor of 2x by platform
	        if( a >= 2200000 ) {
                    mydb.dropDatabase();
		    return;
		}
            }
        } 
        print("count: " + t.count());

        var res = t.validate();
        if( !res.valid ) {
            print("32bit.js FAIL validating");
            print(res.result);
            printjson(res);
	        //mydb.dropDatabase();
	        throw "fail validating 32bit.js";
        }

        mydb.dropDatabase();    
    }

    print("32bit.js SUCCESS");
}

var h = (new Date()).getHours();
if( forceSeedToBe || h <= 4 || h >= 22 ) {
    /* this test is slow, so don't run during the day */
    print("\n32bit.js running - this test is slow only runs at night."); 
    f();
}
