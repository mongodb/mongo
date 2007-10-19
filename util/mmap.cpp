// mmap.cpp

#include "stdafx.h"
#include "mmap.h"

#if defined(_WIN32) 

#include "windows.h"

MemoryMappedFile::MemoryMappedFile() {
	fd = 0; maphandle = 0; view = 0;
}

MemoryMappedFile::~MemoryMappedFile() {
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
 //const std::basic_string<TCHAR> s) {
  std::basic_ostringstream<TCHAR> buf;
  buf << s;
  return buf.str();
}

void* MemoryMappedFile::map(const char *filename, int length) {
	std::wstring filenamew = toWideString(filename);

	fd = CreateFile(
		filenamew.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if( fd == INVALID_HANDLE_VALUE ) {
		cout << "CreateFile failed " << filename << endl;
		return 0;
	}

	maphandle = CreateFileMapping(fd, NULL, PAGE_READWRITE, 0, length, NULL);
	if( maphandle == NULL ) {
		cout << "CreateFileMapping failed " << filename << endl;
		return 0;
	}

	view = MapViewOfFile(maphandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if( view == 0 )
		cout << "MapViewOfFile failed " << filename << endl;

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
}

MemoryMappedFile::~MemoryMappedFile() {
	if( view )
		munmap(view, len);
	view = 0;

	if( fd )
		close(fd);
	fd = 0;
}

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
		cout << "map: file length=" << filelen << " want:" << length << endl;
		if( filelen != 0 ) { 
			cout << "  failing mapping" << endl;
			return 0;
		}
		cout << "  writing file to full length with zeroes..." << endl;
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
		cout << "  done" << endl;
	}

	lseek(fd, length, SEEK_SET);
	write(fd, "", 1);

	view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	return view;
}

void MemoryMappedFile::flush(bool sync) {
	if( msync(view, len, sync ? MS_SYNC : MS_ASYNC) )
		cout << "msync error " << errno << endl;
}

#endif

