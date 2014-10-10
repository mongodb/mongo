// tests to make sure that tag ranges are added/removed/updated successfully

s = new ShardingTest( "tag_range" , 2 , 0 , 1 , { nopreallocj : true } );

// this set up is not required but prevents warnings in the remove
db = s.getDB( "tag_range" );

s.adminCommand( { enableSharding : "test" } )
s.adminCommand( { shardCollection : "test.tag_range" , key : { _id : 1 } } );

assert.eq( 1 , s.config.chunks.count() );

sh.addShardTag( "shard0000" , "a" )

// add two ranges, verify the additions

sh.addTagRange( "test.tag_range" , { _id : 5 } , { _id : 10 } , "a" );
sh.addTagRange( "test.tag_range" , { _id : 10 } , { _id : 15 } , "b" );

assert.eq( 2 , s.config.tags.count() , "tag ranges were not successfully added" );

// remove the second range, should be left with one

sh.removeTagRange( "test.tag_range" , { _id : 10 } , { _id : 15 } , "b" );

assert.eq( 1 , s.config.tags.count() , "tag range not removed successfully" );

// the additions are actually updates, so you can alter a range's max
sh.addTagRange( "test.tag_range" , { _id : 5 } , { _id : 11 } , "a" );

assert.eq( 11 , s.config.tags.findOne().max._id , "tag range not updated successfully" );

s.stop();

