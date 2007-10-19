/* storage.h

   Storage subsystem management.
   Lays out our datafiles on disk, manages disk space.
*/

#pragma once

#pragma pack(push)
#pragma pack(1)

class DiskLoc {
	int reserved; /* this will be volume, file #, etc. */
	int ofs;
public:
	DiskLoc() { reserved = -1; ofs = -1; }

	bool isNull() { return ofs == -1; }
	void Null() { reserved = -1; ofs = -1; }
	void assertOk() { assert(!isNull()); }

	int getOfs() { return ofs; }
	void setOfs(int _ofs) { 
		reserved = -2;
		ofs = _ofs;
	}

	void inc(int amt) {
		assert( !isNull() );
		ofs += amt;
	}

	bool sameFile(DiskLoc b) { return reserved == b.reserved; /* not really done...*/ }

};

#pragma pack(pop)
