// from server 2326 - make sure that sharding only works with unique indices

s = new ShardingTest( "shard_index", 2, 0, 1 )

// Regenerate fully because of SERVER-2782
for ( var i = 0; i < 19; i++ ) {
		
	var coll = s.admin._mongo.getDB( "test" ).getCollection( "foo" + i )
	coll.drop()

	for ( var j = 0; j < 300; j++ ) {
		coll.insert( { num : j, x : 1 } )
	}
    assert.eq( null, coll.getDB().getLastError() );

	if(i == 0) s.adminCommand( { enablesharding : "" + coll._db } );

	print("\n\n\n\n\nTest # " + i)
	
	if ( i == 0 ) {

		// Unique index exists, but not the right one.
		coll.ensureIndex( { num : 1 }, { unique : true } )
		coll.ensureIndex( { x : 1 } )

		passed = false
		try {
			s.adminCommand( { shardcollection : "" + coll, key : { x : 1 } } )
			passed = true
		} catch (e) {
			print( e )
		}
		assert( !passed, "Should not shard collection when another unique index exists!")

	}
	if ( i == 1 ) {
		
		// Unique index exists as prefix, also index exists
		coll.ensureIndex( { x : 1 } )
		coll.ensureIndex( { x : 1, num : 1 }, { unique : true } )
		
		try{
			s.adminCommand({ shardcollection : "" + coll, key : { x : 1 } })
		}
		catch(e){
			print(e)
			assert( false, "Should be able to shard non-unique index without unique option.")
		}
		
	}
	if ( i == 2 ) {
		// Non-unique index exists as prefix, also index exists.  No unique index.
		coll.ensureIndex( { x : 1 } )
		coll.ensureIndex( { x : 1, num : 1 } )

        passed = false;
		try{
			s.adminCommand({ shardcollection : "" + coll, key : { x : 1 } })
            passed = true;

		}
		catch( e ){
			print(e)
			assert( !passed, "Should be able to shard collection with no unique index if unique not specified.")
		}
	}
	if ( i == 3 ) {

		// Unique index exists as prefix, also unique index exists
		coll.ensureIndex( { num : 1 }, { unique : true })
		coll.ensureIndex( { num : 1 , x : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with unique prefix index.")
		}

	}
	if ( i == 4 ) {

		// Unique index exists as id, also unique prefix index exists
		coll.ensureIndex( { _id : 1, num : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { _id : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with unique id index.")
		}
		
	}
	if ( i == 5 ) {

		// Unique index exists as id, also unique prefix index exists
		coll.ensureIndex( { _id : 1, num : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { _id : 1, num : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with unique combination id index.")
		}
		
	}
	if ( i == 6 ) {

		coll.remove({})
		
		// Unique index does not exist, also unique prefix index exists
		coll.ensureIndex( { num : 1, _id : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with no unique index but with a unique prefix index.")
		}
		
        printjson( coll.getIndexes() )
        
        // Make sure the index created is unique!
        assert.eq( 1, coll.getDB().getCollection( "system.indexes" ).find( { ns : "" + coll, key : { num : 1 }, unique : true } ).itcount() )
		
	}
    if ( i == 7 ) {
		coll.remove({})

		// No index exists

		try{
		    assert.eq( coll.find().itcount(), 0 )
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 } })
		}
		catch( e ){
			print(e)
            assert( false, "Should be able to shard collection with no index on shard key.")
		}
	}
    if ( i == 8 ) {
		coll.remove({})

		// No index exists

        passed = false
		try{
		    assert.eq( coll.find().itcount(), 0 )
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 }, unique : true })
            passed = true
		}
		catch( e ){
			print(e)
		}
        assert( passed, "Should be able to shard collection with unique flag but with no unique index on shard key, if coll empty.")
        
        printjson( coll.getIndexes() )
        
        // Make sure the index created is unique!
        assert.eq( 1, coll.getDB().getCollection( "system.indexes" ).find( { ns : "" + coll, key : { num : 1 }, unique : true } ).itcount() )
	}
    if ( i == 9 ) {

		// Unique index exists on a different field as well
		coll.ensureIndex( { num : 1 }, { unique : true } )
		coll.ensureIndex( { x : 1 } )

        passed = false
		try {
			s.adminCommand( { shardcollection : "" + coll, key : { x : 1 } } )
            passed = true
		} catch (e) {
			print( e )
		}
        assert( !passed, "Should not shard collection when another unique index exists!" )
	}
    if ( i == 10 ){

        //try sharding non-empty collection without any index
        passed = false;
        try{
            s.adminCommand( { shardcollection : "" + coll, key : { num : 1 } } );
            passed = true;
        }
        catch( e ){
            print(e);
        }
        assert( !passed , "Should not be able to shard without index");

        //now add containing index and try sharding by prefix
        coll.ensureIndex( {num : 1, x : 1} );

		try{
			s.adminCommand( { shardcollection : "" + coll, key : { num : 1 } } );
			passed = true;
		}
		catch( e ){
			print(e);
		}
		assert( passed , "Should be able to shard collection with prefix of existing index");

        printjson( coll.getIndexes() );

        //make sure no extra index is created
        assert.eq( 2, coll.getIndexes().length );
    }
    if ( i == 11 ){
        coll.remove({})

        //empty collection with useful index. check new index not created
        coll.ensureIndex( {num : 1, x : 1} );

        try{
            s.adminCommand( { shardcollection : "" + coll, key : { num : 1 } } );
            passed = true;
        }
        catch( e ){
            print(e);
        }
        assert( passed , "Should be able to shard collection with prefix of existing index");

        printjson( coll.getIndexes() );

        //make sure no extra index is created
        assert.eq( 2, coll.getIndexes().length );
    }
    if ( i == 12 ){

        //check multikey values for x make index unusable for shard key
        coll.save({num : 100 , x : [2,3] });
        coll.ensureIndex( {num : 1, x : 1} );

        passed = false;
        try{
            s.adminCommand( { shardcollection : "" + coll, key : { num : 1 } } );
            passed = true;
        }
        catch( e ){
            print(e);
        }
        assert( !passed , "Should not be able to shard collection with mulikey index");
    }
    if ( i == 13 ){

        coll.save({ num : [100, 200], x : 10});
        coll.ensureIndex( { num : 1, x : 1} );

        passed = false;
        try{
            s.adminCommand( { shardcollection : "" + coll, key : { num : 1 } } );
            passed = true;
        }
        catch( e ){
            print(e);
        }
        assert( !passed , "Should not be able to shard collection with mulikey index");

    }
    if ( i == 14 ){

        coll.save({ num : 100, x : 10, y : [1,2]});
        coll.ensureIndex( { num : 1, x : 1, y : 1} );

        passed = false;
        try{
            s.adminCommand( { shardcollection : "" + coll, key : { num : 1 } } );
            passed = true;
        }
        catch( e ){
            print(e);
        }
        assert( !passed , "Should not be able to shard collection with mulikey index");

    }
    if ( i == 15 ) {

        // try sharding with a hashed index
        coll.ensureIndex( { num : "hashed"} );

        try{
            s.adminCommand( { shardcollection : "" + coll, key : { num : "hashed" } } );
        }
        catch( e ){
            print(e);
            assert( false, "Should be able to shard collection with hashed index.");
        }
    }
    if ( i == 16 ) {

        // create hashed index, but try to declare it unique when sharding
        coll.ensureIndex( { num : "hashed"} )

        passed = false;
        try{
            s.adminCommand({ shardcollection : "" + coll, key : { num : "hashed" }, unique : true});
            passed = true;
        }
        catch( e ){
            print(e);
        }
        assert( !passed , "Should not be able to declare hashed shard key unique.");

    }
    if ( i == 17 ) {

        // create hashed index, but unrelated unique index present
        coll.ensureIndex( { x : "hashed" } );
        coll.ensureIndex( { num : 1 }, { unique : true} );

        passed = false;
        try {
            s.adminCommand( { shardcollection : "" + coll, key : { x : "hashed" } } );
            passed = true;
        }
        catch (e) {
            print( e );
        }
        assert( !passed, "Should not be able to shard on hashed index with another unique index" );

    }
    if ( i == 18 ) {

        // create hashed index, and a regular unique index exists on same field
        coll.ensureIndex( { num : "hashed" } );
        coll.ensureIndex( { num : 1 }, { unique : true } );

        try{
            s.adminCommand({ shardcollection : "" + coll, key : { num : "hashed" } } );
        }
        catch( e ){
            print(e);
            assert( false, "Should be able to shard coll with hashed and regular unique index");
        }
    }
}

s.stop();
