// mmap.h

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

private:
	HANDLE fd;
	HANDLE maphandle;
	void *view;
	int len;
};
