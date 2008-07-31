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

/* tests.cpp

   unit test & such
*/

#include "stdafx.h"
#include "../util/mmap.h"

int test2_old() { 
	cout << "test2" << endl;
	printStackTrace();
	if( 1 ) 
		return 1;

	MemoryMappedFile f;

	char *p = (char *) f.map("/tmp/test.dat", 64*1024*1024);
	char *start = p;
	char *end = p + 64*1024*1024-2;
	end[1] = 'z';
	int i;
	while( p < end ) { 
		*p++ = ' ';
		if( ++i%64 == 0 ) { *p++ = '\n'; *p++ = 'x'; }
	}
	*p = 'a';

	f.flush(true);
	cout << "done" << endl;

	char *x = start + 32 * 1024 * 1024;
	char *y = start + 48 * 1024 * 1024;
	char *z = start + 62 * 1024 * 1024;

	strcpy(z, "zfoo");
	cout << "y" << endl;
	strcpy(y, "yfoo");
	strcpy(x, "xfoo");
	strcpy(start, "xfoo");

	exit(3);

	return 1;
}
