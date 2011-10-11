
assert( ObjectId.isValid('000000000000000000000000') )
assert( ObjectId.isValid('ffffffffffffffffffffffff') )
assert( ObjectId.isValid('FFFFFFFFFFFFFFFFFFFFFFFF') )
assert( ObjectId.isValid('afAFafAFafAFfaFafAFafAfa') )
assert( ObjectId.isValid('390399890283084209843092') )

assert( !ObjectId.isValid('3903998902830842098430929') )
assert( !ObjectId.isValid('00000000000000000000000G') )
assert( !ObjectId.isValid('00000000000000000000000') )
