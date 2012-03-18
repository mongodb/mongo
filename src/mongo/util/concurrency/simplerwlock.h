
#include "bson/util/atomic_int.h"

namespace mongo {

    /** separated out as later the implementation of this may be different than RWLock, 
        depending on OS, as there is no upgrade etc. facility herein.
    */
    class SimpleRWLock : boost::noncopyable { 
        RWLockBase m;
#if defined(_WIN32) && defined(_DEBUG)
        AtomicUInt shares;
        ThreadLocalValue<int> s;
        unsigned tid;
#endif
    public:
        explicit SimpleRWLock(const char *) { }
        SimpleRWLock() { }
        void lock();
        void unlock();
        void lock_shared();
        void unlock_shared();
        class Shared : boost::noncopyable {
            SimpleRWLock& _r;
        public:
            Shared(SimpleRWLock& rwlock) : _r(rwlock) {_r.lock_shared(); }
            ~Shared() { _r.unlock_shared(); }
        };
        class Exclusive : boost::noncopyable {
            SimpleRWLock& _r;
        public:
            Exclusive(SimpleRWLock& rwlock) : _r(rwlock) {_r.lock(); }
            ~Exclusive() { _r.unlock(); }
        };
    };

}
