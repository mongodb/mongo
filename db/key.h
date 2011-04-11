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
    private:
        BSONObj _o;
    };

    class KeyV1 { 
    public:
        ~KeyV1() { 
            DEV _keyData = 0; 
        }
        KeyV1();
        explicit KeyV1(const char *keyData);
        int woCompare(const KeyV1& r, const Ordering &o) const;
        bool woEqual(const KeyV1& r) const;
        BSONObj toBson() const;
        string toString() const;
        int dataSize() const;
        const char * data() const;
        BSONElement _firstElement() const { 
            assert( _keyData == 0 );
            return _o.firstElement(); 
        }
    protected:
        unsigned char *_keyData;
        BSONObj _o;
    private:
        int compareHybrid(const KeyV1& right, const Ordering& order) const;
    };

    class KeyV1Owned : public KeyV1 { 
        KeyV1Owned(const KeyV1Owned&); //not copyable
    public:
        KeyV1Owned(const BSONObj& obj);
        ~KeyV1Owned() { delete[] _keyData; }
    };

    typedef KeyBson Key;
    typedef KeyBson KeyOwned;
    //typedef KeyV1 Key;
    //typedef KeyV1Owned KeyOwned;

};
