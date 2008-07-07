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
class BtreeBucket;
class JSObj;
class PhysicalDataFile;

class DiskLoc {
	int fileNo; /* this will be volume, file #, etc. */
	int ofs;
public:
	enum { NullOfs = -1, MaxFiles=4000 };
	int a() const { return fileNo; }
	DiskLoc(int a, int b) : fileNo(a), ofs(b) { 
		assert(ofs!=0);
	}
	DiskLoc() { fileNo = -1; ofs = NullOfs; }

	DiskLoc(const DiskLoc& l) { fileNo=l.fileNo; ofs=l.ofs; }

	bool questionable() { 
	  return ofs < -1 ||
	    fileNo < -1 ||
	    fileNo > 20;
	}

	bool isNull() const { return ofs == NullOfs; }
	void Null() { fileNo = -1; ofs = NullOfs; }
	void setInvalid() { fileNo = -2; }
	void assertOk() { assert(!isNull()); }

	string toString() const {
		if( isNull() ) 
			return "null";
		stringstream ss;
		ss << hex << fileNo << ':' << ofs;
		return ss.str();
	}

	int& GETOFS() { return ofs; }
	int getOfs() const { return ofs; }
	void set(int a, int b) { fileNo=a; ofs=b; }
	void setOfs(int _fileNo, int _ofs) { 
		fileNo = _fileNo;
		ofs = _ofs;
	}

	void inc(int amt) {
		assert( !isNull() );
		ofs += amt;
	}

	bool sameFile(DiskLoc b) { return fileNo == b.fileNo; }

	bool operator==(const DiskLoc& b) const { return fileNo==b.fileNo && ofs == b.ofs; }
	bool operator!=(const DiskLoc& b) const { return !(*this==b); }
	const DiskLoc& operator=(const DiskLoc& b) { 
		fileNo=b.fileNo; ofs = b.ofs;
		assert(ofs!=0);
		return *this;
	}
	int compare(const DiskLoc& b) const { 
		int x = fileNo - b.fileNo;
		if( x ) 
			return x;
		if( ofs == b.ofs ) return 0;
		return ofs < b.ofs ? -1 : 1;
	}
	bool operator<(const DiskLoc& b) const { 
		if( fileNo == b.fileNo )
			return ofs < b.ofs;
		return fileNo < b.fileNo;
	}

	/* get the "thing" associated with this disk location.
	   it is assumed the object is what it is -- you must asure that: 
	   think of this as an unchecked type cast.
    */
	JSObj obj() const;
	Record* rec() const;
	DeletedRecord* drec() const;
	Extent* ext() const;
	BtreeBucket* btree() const;

	PhysicalDataFile& pdf() const;
};

#pragma pack(pop)
