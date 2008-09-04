/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "goodies.h"

Nullstream nullstream;
Logstream logstream;
boost::mutex Logstream::mutex;

unsigned occasion = 0;
bool goingAway = false;

bool isPrime(int n) { 
	int z = 2;
	while( 1 ) { 
		if( z*z > n )
			break;
		if( n % z == 0 )
			return false;
		z++;
	}
	return true;
}

int nextPrime(int n) { 
	n |= 1; // 2 goes to 3...don't care...
	while( !isPrime(n) )
		n += 2;
	return n;
}

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

		assert( isPrime(3) );
		assert( isPrime(2) );
		assert( isPrime(13) );
		assert( isPrime(17) );
		assert( !isPrime(9) );
		assert( !isPrime(6) );
		assert( nextPrime(4) == 5 );
		assert( nextPrime(8) == 11 );

		assert( endsWith("abcde", "de") );
		assert( !endsWith("abcde", "dasdfasdfashkfde") );

		assert( swapEndian(0x01020304) == 0x04030201 );

	}
} utilTest;
