// mmap.cpp

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
#include "mmap.h"

set<MemoryMappedFile*> mmfiles;

MemoryMappedFile::~MemoryMappedFile() { 
	close(); 
	mmfiles.erase(this);
}

/*static*/ 
int closingAllFiles = 0;
void MemoryMappedFile::closeAllFiles() { 
	if( closingAllFiles ) {
		cout << "warning closingAllFiles=" << closingAllFiles << endl;
		return;
	}
	++closingAllFiles;
	for( set<MemoryMappedFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ )
		(*i)->close();
	log() << "  closeAllFiles() finished" << endl;
	--closingAllFiles;
}

#if defined(_WIN32) 

#include "windows.h"

MemoryMappedFile::MemoryMappedFile() {
	fd = 0; maphandle = 0; view = 0;
	mmfiles.insert(this);
}

void MemoryMappedFile::close() {
	if( view )
		UnmapViewOfFile(view);
	view = 0;
	if( maphandle )
		CloseHandle(maphandle);
	maphandle = 0;
	if( fd )
		CloseHandle(fd);
	fd = 0;
}

std::wstring toWideString(const char *s) {
  std::basic_ostringstream<TCHAR> buf;
  buf << s;
  return buf.str();
}

unsigned mapped = 0;

void* MemoryMappedFile::map(const char *filename, int length) {
	std::wstring filenamew = toWideString(filename);

	fd = CreateFile(
		filenamew.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if( fd == INVALID_HANDLE_VALUE ) {
		cout << "CreateFile failed " << filename << endl;
		return 0;
	}

#if defined(_WIN32)
	if( mapped > 500000000 ) { 
		cout << "WARNING: too much mem mapped for win32" << endl;
//		if( length > 50000000 )
//			length = 50000000;
	}
	mapped += length;
#endif

	maphandle = CreateFileMapping(fd, NULL, PAGE_READWRITE, 0, length, NULL);
	if( maphandle == NULL ) {
		cout << "CreateFileMapping failed " << filename << endl;
		return 0;
	}

	view = MapViewOfFile(maphandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if( view == 0 ) {
		cout << "MapViewOfFile failed " << filename << " errno:";
		cout << GetLastError();
		cout << endl;
	}

	return view;
}

void MemoryMappedFile::flush(bool) {
}

#else

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

MemoryMappedFile::MemoryMappedFile() {
	fd = 0; maphandle = 0; view = 0; len = 0;
	mmfiles.insert(this);
}

void MemoryMappedFile::close() {
	if( view )
		munmap(view, len);
	view = 0;

	if( fd )
		::close(fd);
	fd = 0;
}

#ifndef O_NOATIME
#warning NO O_NOATIME
#define O_NOATIME 0
#endif

void* MemoryMappedFile::map(const char *filename, int length) {
	len = length;

    fd = open(filename, O_CREAT | O_RDWR | O_NOATIME, S_IRUSR | S_IWUSR);
	if( !fd ) {
		cout << "couldn't open " << filename << ' ' << errno << endl;
		return 0;
	}

	/* make sure the file is the full desired length */
	off_t filelen = lseek(fd, 0, SEEK_END);
	if( filelen < length ) { 
		log() << "map: file length=" << (unsigned) filelen << " want:"
		 << length
		 << endl;
		if( filelen != 0 ) { 
			log() << "  failing mapping" << endl;
			return 0;
		}
		log() << "  writing file to full length with zeroes..." << endl;
		int z = 8192;
		char buf[z];
		memset(buf, 0, z);
		int left = length;
		while( 1 ) { 
			if( left <= z ) {
				write(fd, buf, left);
				break;
			}
			write(fd, buf, z);
			left -= z;
		}
		log() << "  done" << endl;
	}

	lseek(fd, length, SEEK_SET);
	write(fd, "", 1);

	view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if( view == MAP_FAILED ) { 
		cout << "  mmap() failed for " << filename << " len:" << length << " errno:" << errno << endl;
		return 0;
	}
	return view;
}

void MemoryMappedFile::flush(bool sync) {
	if( msync(view, len, sync ? MS_SYNC : MS_ASYNC) )
		problem() << "msync error " << errno << endl;
}

#endif
