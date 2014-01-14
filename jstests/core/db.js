function testInvalidDBNameThrowsExceptionWithConstructor() {
    assert.throws( function() { return new DB( null, "/\\" ); } );
}

function testInvalidDBNameThrowsExceptionWithSibling() {
    assert.throws( function() { return db.getSiblingDB( "/\\" ); } );
}

testInvalidDBNameThrowsExceptionWithConstructor();
testInvalidDBNameThrowsExceptionWithSibling();

