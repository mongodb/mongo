// Value::coerceToBool() is consistent with BSONElement::trueValue().  SERVER-6120

t = db.jstests_aggregation_server6120;
t.drop();

t.save( {object: {a:1}} );

function coerceToBool( value ) {
    return t.aggregate( { $project:{ boolValue:{ $and:[ value ] } } } ).result[ 0 ].boolValue;
}

function assertBoolValue( expectedBool, value ) {
    assert.eq( expectedBool, coerceToBool( value ) );
}

// Bool type.
assertBoolValue( false, false );
assertBoolValue( true, true );

// Numeric types.
assertBoolValue( false, NumberLong( 0 ) );
assertBoolValue( true, NumberLong( 1 ) );
assertBoolValue( false, NumberInt( 0 ) );
assertBoolValue( true, NumberInt( 1 ) );
assertBoolValue( false, 0.0 );
assertBoolValue( true, 1.0 );

// Always false types.
assertBoolValue( false, null );

// Always true types.
assertBoolValue( true, '' );
assertBoolValue( true, 'a' );
assertBoolValue( true, "$object" );
assertBoolValue( true, [] );
assertBoolValue( true, [ 1 ] );
assertBoolValue( true, new ObjectId() );
assertBoolValue( true, new Date() );
assertBoolValue( true, /a/ );
assertBoolValue( true, new Timestamp() );

// Missing field.
assertBoolValue( false, '$missingField' );
