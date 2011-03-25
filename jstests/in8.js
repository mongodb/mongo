t = db.jstests_in8;
t.drop(); 

// SERVER-2829
if ( 0 ) {
t.save( {key: []} ); 
t.save( {key: [1]} ); 
t.save( {key: ['1']} ); 
t.save( {key: null} ); 
t.save( {} ); 

function doTest() { 
    assert.eq( 1, t.count( {key:[1]} ) ); 
    assert.eq( 1, t.count( {key:{$in:[[1]]}} ) ); 
    assert.eq( 1, t.count( {key:['1']} ) ); 
    assert.eq( 1, t.count( {key:{$in:[['1']]}} ) ); 
    assert.eq( 1, t.count( {key:[]} ) ); 
    assert.eq( 1, t.count( {key:{$in:[[]]}} ) ); 
} 

doTest(); 
t.ensureIndex( {i:1} ); 
doTest();
}