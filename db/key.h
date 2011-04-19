// @file key.h

#include "jsobj.h"

namespace mongo { 

    /** Key class for precomputing a small format index key that is denser than a traditional BSONObj. 

        KeyBson is a legacy wrapper implementation for old BSONObj style keys for v:0 indexes.

        KeyV1 is the new implementation.
    */
    class KeyBson { 
    public:
        KeyBson() { }
        explicit KeyBson(const char *keyData) : _o(keyData) { }
        explicit KeyBson(const BSONObj& obj) : _o(obj) { }
        int woCompare(const KeyBson& r, const Ordering &o) const { return _o.woCompare(r._o, o); }
        bool woEqual(const KeyBson& r) const { return _o.woEqual(r._o); }
        BSONObj toBson() const { return _o; }
        string toString() const { return _o.toString(); }
        int dataSize() const { return _o.objsize(); }
        const char * data() const { return _o.objdata(); }
        BSONElement _firstElement() const { return _o.firstElement(); }
        bool isCompactFormat() const { false; }
    private:
        BSONObj _o;
    };

    class KeyV1 { 
    public:
        ~KeyV1() { 
            DEV _keyData = 0; 
        }
        KeyV1() { _keyData = 0; }

        /** @param keyData can be a buffer containing data in either BSON format, OR in KeyV1 format. 
                   when BSON, we are just a wrapper
        */
        explicit KeyV1(const char *keyData) {
            const unsigned char *p = (const unsigned char *) keyData;
            if( *p & 0x80 ) { 
                _keyData = p;
            }
            else { 
                // traditional bson format
                _keyData = 0;
                _o = BSONObj(keyData);
            }
        }
        int woCompare(const KeyV1& r, const Ordering &o) const;
        bool woEqual(const KeyV1& r) const;
        BSONObj toBson() const;
        string toString() const { return toBson().toString(); }
        int dataSize() const;
        const char * data() const { 
            return _keyData != 0 ? (const char *) _keyData : _o.objdata();
        }

        /** only used by geo, which always has bson keys */
        BSONElement _firstElement() const { 
            assert( _keyData == 0 );
            return _o.firstElement(); 
        }
        bool isCompactFormat() const { return _keyData != 0; }
    protected:
        const unsigned char *_keyData;
        BSONObj _o;
    private:
        int compareHybrid(const KeyV1& right, const Ordering& order) const;
    };

    class KeyV1Owned : public KeyV1 { 
        KeyV1Owned(const KeyV1Owned&); //not copyable
    public:
        /** @obj a BSON object to be translated to KeyV1 format.  If the object isn't 
                 representable in KeyV1 format (which happens, intentionally, at times)
                 it will stay as bson herein.
        */
        KeyV1Owned(const BSONObj& obj);
        ~KeyV1Owned() { free((void*) _keyData); }
    };

    //typedef KeyBson Key;
    //typedef KeyBson KeyOwned;
    typedef KeyV1 Key;
    typedef KeyV1Owned KeyOwned;

};
