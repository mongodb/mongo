// mmap.h

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

#pragma once

class MemoryMappedFile {
public:
	static void closeAllFiles();
	MemoryMappedFile();
	~MemoryMappedFile(); /* closes the file if open */
	void close();

	/* only smart enough right now to deal with files of a fixed length. 
	   creates if DNE
	*/
	void* map(const char *filename, int length);

	void flush(bool sync);

	void* viewOfs() { return view; }

private:
	HANDLE fd;
	HANDLE maphandle;
	void *view;
	int len;
};
