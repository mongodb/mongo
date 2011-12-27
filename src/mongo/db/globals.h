// @file globals.h
// grouping of global variables to make concurrency work clearer

#pragma once

namespace mongo {

    void assertStartingUp();

    // this is prototype for now, we'll see if it is helpful

    /** "value is Const After Server Init" helper
    *
    * Example:
    *
    *  casi<int> foo = 3;
    *  foo.ref() = 4; // asserts if not still in server init
    *  int x = foo+1; // ok anytime
    *
    */
    template< class T >
    class casi : boost::noncopyable {
        T val;
    public:
        casi(const T& t) : val(t) { 
            DEV assertStartingUp();
        }
        operator const T& () { return val; }
        T& ref() { 
            DEV assertStartingUp();
            return val; 
        }
    };

    /** partially specialized for cases where out global variable is a pointer -- we want the value
     * pointed at to be constant, not just the pointer itself
     */
    template< typename T >
    class casi<T*> : boost::noncopyable {
        T * val;
        void operator=(T*);
    public:
        casi(T* t) : val(t) { 
            DEV assertStartingUp();
        }
        operator const T* () { return val; }
        const T* get() { return val; }
        T*& ref() { 
            DEV assertStartingUp();
            return val; 
        }
    };

}
