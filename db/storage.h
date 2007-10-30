/* storage.h

   Storage subsystem management.
   Lays out our datafiles on disk, manages disk space.
*/

#pragma once

#pragma pack(push)
#pragma pack(1)

class Record;
class DeletedRecord;
class Extent;

class DiskLoc {
	int reserved; /* this will be volume, file #, etc. */
	int ofs;
public:
	enum { NullOfs = -1 };
	int a() const { return reserved; }
	DiskLoc(int a, int b) : reserved(a), ofs(b) { }
	DiskLoc() { reserved = -1; ofs = NullOfs; }

	bool isNull() { return ofs == NullOfs; }
	void Null() { reserved = -1; ofs = NullOfs; }
	void assertOk() { assert(!isNull()); }

	int getOfs() const { return ofs; }
	void set(int a, int b) { reserved=a; ofs=b; }
	void setOfs(int _ofs) { 
		reserved = -2; /*temp: fix for multiple datafiles */
		ofs = _ofs;
	}

	void inc(int amt) {
		assert( !isNull() );
		ofs += amt;
	}

	bool sameFile(DiskLoc b) { return reserved == b.reserved; /* not really done...*/ }

	bool operator==(const DiskLoc& b) { return reserved==b.reserved && ofs == b.ofs; }

	Record* rec() const;
	DeletedRecord* drec() const;
	Extent* ext() const;
};

#pragma pack(pop)
