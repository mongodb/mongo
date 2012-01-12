#pragma once

namespace mongo {

    /** Thread safe map.  
        Be careful not to use this too much or it could make things slow;
        if not a hot code path no problem.
    
        Examples:

        mapsf<int,int> mp;

        int x = mp.get();

        map<int,int> two;
        mp.swap(two);

        {
            mapsf<int,int>::ref r(mp);
            r[9] = 1;
            map<int,int>::iterator i = r.r.begin();
        }
        
    */
    template< class K, class V >
    struct mapsf : boost::noncopyable {
        SimpleMutex m;
        map<K,V> val;
        friend struct ref;
    public:
        mapsf() : m("mapsf") { }
        void swap(map<K,V>& rhs) {
            SimpleMutex::scoped_lock lk(m);
            val.swap(rhs);
        }
        bool empty() { 
            SimpleMutex::scoped_lock lk(m);
            return val.empty(); 
        }
        // safe as we pass by value:
        V get(K k) { 
            SimpleMutex::scoped_lock lk(m);
            typename map<K,V>::iterator i = val.find(k);
            if( i == val.end() )
                return V();
            return i->second;
        }
        // think about deadlocks when using ref.  the other methods
        // above will always be safe as they are "leaf" operations.
        struct ref {
            SimpleMutex::scoped_lock lk;
        public:
            map<K,V> &r;
            ref(mapsf<K,V> &m) : lk(m.m), r(m.val) { }
            V& operator[](const K& k) { return r[k]; }
        };
    };

}
