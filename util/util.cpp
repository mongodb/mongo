#include "stdafx.h"
#include "goodies.h"

bool goingAway = false;

struct UtilTest { 
	UtilTest() { 
		assert( WrappingInt(0) <= WrappingInt(0) );
		assert( WrappingInt(0) <= WrappingInt(1) );
		assert( !(WrappingInt(1) <= WrappingInt(0)) );
		assert( (WrappingInt(0xf0000000) <= WrappingInt(0)) );
		assert( (WrappingInt(0xf0000000) <= WrappingInt(9000)) );
		assert( !(WrappingInt(300) <= WrappingInt(0xe0000000)) );

		assert( tdiff(3, 4) == 1 ); 
		assert( tdiff(4, 3) == -1 ); 
		assert( tdiff(0xffffffff, 0) == 1 ); 
	}
} utilTest;
