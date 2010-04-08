/**
*    Copyright (C) 2008 10gen Inc.info
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
#include "mmap.h"

namespace mongo {

	//extern bool checkNsFilesOnLoad;

static set<RamStoreFile*> files;

void RamStoreFile::grow(int offset, int len) {
	cout << "GROW ofs:" << offset << " len:" << len;
	assert( len > 0 );
	Node& n = _m[offset];
	cout << " oldlen:" << n.len << endl;
	assert( n.len > 0 );
	if( len > n.len ) { 
		n.p = (char *) realloc(n.p, len);
		memset(((char *)n.p) + n.len, 0, len - n.len);
		n.len = len;
	}
}

/* maxLen can be -1 for existing data */
void* RamStoreFile::at(int offset, int maxLen) {
    if( offset != _last ) {
        if( _m.count(_last) ) {
            _m[_last].check();
            if( !(offset < _last || offset >= _last + _m[_last].len) ) {
                cout << offset << ' ' << _last << ' ' << _m[_last].len << endl;
                assert(false);
            }
        }
    }
    _last = offset;

    Node& n = _m[offset];
    if( n.len == 0 ) { 
        // create
        if( strstr(name, ".ns") == 0 )
                cout << "CREATE " << name << " ofs:" << offset << " len:" << maxLen << endl;
            assert( maxLen >= 0 );
            n.p = (char *) calloc(maxLen+1, 1);
            n.len = maxLen;
        }
        assert( n.len >= maxLen );
        n.check();
        return n.p;
    }

void RamStoreFile::Node::check() { 
    assert( p[len] == 0 );
}

void RamStoreFile::check() { 
    for( std::map<int,Node>::iterator i = _m.begin(); i != _m.end(); i++ ) { 
        i->second.check();
    }
}

void RamStoreFile::validate() {
    for( set<RamStoreFile*>::iterator i = files.begin(); i != files.end(); i++ ) { 
        (*i)->check();
    }
}

RamStoreFile::~RamStoreFile() { 
    check();
    files.erase(this);
}

RamStoreFile::RamStoreFile() : _len(0) { 
	//    checkNsFilesOnLoad = false;
    files.insert(this);
}

}

