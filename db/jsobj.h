// jsobj.h

#pragma once

#include "../stdafx.h"
#include "pdfile.h"

#pragma pack(push)
#pragma pack(1)

/* BinData = binary data types. 
   EOO = end of object
*/
enum JSType { EOO = 0, Number=1, String=2, Object=3, Array=4, BinData=5, Undefined=6, jstOID=7, Bool=8, Date=9 , jstNULL=10 };

/* subtypes of BinData. 
   bdtCustom and above are ones that the JS compiler understands, but are
   opaque to the database.
*/
enum BinDataType { Function=1, ByteArray=2, bdtCustom=128 };

/*	Object id's are optional for JSObjects.  
	When present they should be the first object member added.
*/
struct OID { 
	long long a;
	unsigned b;
	bool operator==(const OID& r) { return a==r.a&&b==r.b; }
};

/* marshalled js object format:

   <unsigned totalSize> {<byte JSType><string FieldName><Data>}* EOO
      totalSize includes itself.

   Data:
     EOO: nothing follows
     Undefined: nothing follows
     OID: an OID object
     Number: <double>
     String: <unsigned strlen><string>       
     Object: a nested object, which terminates with EOO.
     Array:
       <unsigned length>
       {Object}[length]  
       a nested object, which is the object properties of the array  
     BinData:
       <byte subtype>
       <unsigned len>
       <byte[len] data>
*/

/* db operation message format 

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   
   Update:
      int reserved;
      string collection;  // name of the collection (namespace)
      a series of JSObjects terminated with a null object (i.e., just EOO)
   Insert:
      int reserved;
      string collection;
      a series of JSObjects terminated with a null object (i.e., just EOO)
   Query: see query.h
*/

#pragma pack(pop)

class JSObj {
public:
	JSObj(const char *_data) : data(_data) {
		size = *((int*) data);
	}
	JSObj(Record *r) { 
		size = r->netLength();
		data = r->data;
	}

	const char *objdata() { return data + 4; } // skip the length field.
	int objsize() { return size - 4; }

	OID* getOID() {
		const char *p = objdata();
		if( *p != jstOID )
			return 0;
		return (OID *) ++p;
	}

	int size;
	const char *data;
};
