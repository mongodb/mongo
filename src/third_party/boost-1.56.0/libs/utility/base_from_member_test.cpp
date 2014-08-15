//  Boost test program for base-from-member class templates  -----------------//

//  Copyright 2001, 2003 Daryle Walker.  Use, modification, and distribution are
//  subject to the Boost Software License, Version 1.0.  (See accompanying file
//  LICENSE_1_0.txt or a copy at <http://www.boost.org/LICENSE_1_0.txt>.)

//  See <http://www.boost.org/libs/utility/> for the library's home page.

//  Revision History
//  14 Jun 2003  Adjusted code for Boost.Test changes (Daryle Walker)
//  29 Aug 2001  Initial Version (Daryle Walker)

#include <boost/test/minimal.hpp>  // for BOOST_CHECK, main

#include <boost/config.hpp>       // for BOOST_NO_MEMBER_TEMPLATES
#include <boost/cstdlib.hpp>      // for boost::exit_success
#include <boost/noncopyable.hpp>  // for boost::noncopyable

#include <boost/utility/base_from_member.hpp>  // for boost::base_from_member

#include <functional>  // for std::binary_function, std::less
#include <iostream>    // for std::cout (std::ostream, std::endl indirectly)
#include <set>         // for std::set
#include <typeinfo>    // for std::type_info
#include <utility>     // for std::pair, std::make_pair
#include <vector>      // for std::vector


// Control if extra information is printed
#ifndef CONTROL_EXTRA_PRINTING
#define CONTROL_EXTRA_PRINTING  1
#endif


// A (sub)object can be identified by its memory location and its type.
// Both are needed since an object can start at the same place as its
// first base class subobject and/or contained subobject.
typedef std::pair< void *, std::type_info const * >  object_id;

// Object IDs need to be printed
std::ostream &  operator <<( std::ostream &os, object_id const &oi );

// A way to generate an object ID
template < typename T >
  object_id  identify( T &obj );

// A custom comparison type is needed
struct object_id_compare
    : std::binary_function<object_id, object_id, bool>
{
    bool  operator ()( object_id const &a, object_id const &b ) const;

};  // object_id_compare

// A singleton of this type coordinates the acknowledgements
// of objects being created and used.
class object_registrar
    : private boost::noncopyable
{
public:

    #ifndef BOOST_NO_MEMBER_TEMPLATES
    template < typename T >
        void  register_object( T &obj )
            { this->register_object_imp( identify(obj) ); }
    template < typename T, typename U >
        void  register_use( T &owner, U &owned )
            { this->register_use_imp( identify(owner), identify(owned) ); }
    template < typename T, typename U >
        void  unregister_use( T &owner, U &owned )
            { this->unregister_use_imp( identify(owner), identify(owned) ); }
    template < typename T >
        void  unregister_object( T &obj )
            { this->unregister_object_imp( identify(obj) ); }
    #endif

    void  register_object_imp( object_id obj );
    void  register_use_imp( object_id owner, object_id owned );
    void  unregister_use_imp( object_id owner, object_id owned );
    void  unregister_object_imp( object_id obj );

    typedef std::set<object_id, object_id_compare>  set_type;

    typedef std::vector<object_id>  error_record_type;
    typedef std::vector< std::pair<object_id, object_id> >  error_pair_type;

    set_type  db_;

    error_pair_type    defrauders_in_, defrauders_out_;
    error_record_type  overeager_, overkilled_;

};  // object_registrar

// A sample type to be used by containing types
class base_or_member
{
public:
    explicit  base_or_member( int x = 1, double y = -0.25 );
             ~base_or_member();

};  // base_or_member

// A sample type that uses base_or_member, used
// as a base for the main demonstration classes
class base_class
{
public:
    explicit  base_class( base_or_member &x, base_or_member *y = 0,
     base_or_member *z = 0 );

    ~base_class();

private:
    base_or_member  *x_, *y_, *z_;

};  // base_class

// This bad class demonstrates the direct method of a base class needing
// to be initialized by a member.  This is improper since the member
// isn't initialized until after the base class.
class bad_class
    : public base_class
{
public:
     bad_class();
    ~bad_class();

private:
    base_or_member  x_;

};  // bad_class

// The first good class demonstrates the correct way to initialize a
// base class with a member.  The member is changed to another base
// class, one that is initialized before the base that needs it.
class good_class_1
    : private boost::base_from_member<base_or_member>
    , public base_class
{
    typedef boost::base_from_member<base_or_member>  pbase_type;
    typedef base_class                                base_type;

public:
     good_class_1();
    ~good_class_1();

};  // good_class_1

// The second good class also demonstrates the correct way to initialize
// base classes with other subobjects.  This class uses the other helpers
// in the library, and shows the technique of using two base subobjects
// of the "same" type.
class good_class_2
    : private boost::base_from_member<base_or_member, 0>
    , private boost::base_from_member<base_or_member, 1>
    , private boost::base_from_member<base_or_member, 2>
    , public base_class
{
    typedef boost::base_from_member<base_or_member, 0>  pbase_type0;
    typedef boost::base_from_member<base_or_member, 1>  pbase_type1;
    typedef boost::base_from_member<base_or_member, 2>  pbase_type2;
    typedef base_class                                   base_type;

public:
     good_class_2();
    ~good_class_2();

};  // good_class_2

// Declare/define the single object registrar
object_registrar  obj_reg;


// Main functionality
int
test_main( int , char * [] )
{
    BOOST_CHECK( obj_reg.db_.empty() );
    BOOST_CHECK( obj_reg.defrauders_in_.empty() );
    BOOST_CHECK( obj_reg.defrauders_out_.empty() );
    BOOST_CHECK( obj_reg.overeager_.empty() );
    BOOST_CHECK( obj_reg.overkilled_.empty() );

    // Make a separate block to examine pre- and post-effects
    {
        using std::cout;
        using std::endl;

        bad_class  bc;
        BOOST_CHECK( obj_reg.db_.size() == 3 );
        BOOST_CHECK( obj_reg.defrauders_in_.size() == 1 );

        good_class_1  gc1;
        BOOST_CHECK( obj_reg.db_.size() == 6 );
        BOOST_CHECK( obj_reg.defrauders_in_.size() == 1 );

        good_class_2  gc2;
        BOOST_CHECK( obj_reg.db_.size() == 11 );
        BOOST_CHECK( obj_reg.defrauders_in_.size() == 1 );

        BOOST_CHECK( obj_reg.defrauders_out_.empty() );
        BOOST_CHECK( obj_reg.overeager_.empty() );
        BOOST_CHECK( obj_reg.overkilled_.empty() );

        // Getting the addresses of the objects ensure
        // that they're used, and not optimized away.
        cout << "Object 'bc' is at " << &bc << '.' << endl;
        cout << "Object 'gc1' is at " << &gc1 << '.' << endl;
        cout << "Object 'gc2' is at " << &gc2 << '.' << endl;
    }

    BOOST_CHECK( obj_reg.db_.empty() );
    BOOST_CHECK( obj_reg.defrauders_in_.size() == 1 );
    BOOST_CHECK( obj_reg.defrauders_out_.size() == 1 );
    BOOST_CHECK( obj_reg.overeager_.empty() );
    BOOST_CHECK( obj_reg.overkilled_.empty() );

    return boost::exit_success;
}


// Print an object's ID
std::ostream &
operator <<
(
    std::ostream &     os,
    object_id const &  oi
)
{
    // I had an std::ostringstream to help, but I did not need it since
    // the program never screws around with formatting.  Worse, using
    // std::ostringstream is an issue with some compilers.

    return os << '[' << ( oi.second ? oi.second->name() : "NOTHING" )
     << " at " << oi.first << ']';
}

// Get an object ID given an object
template < typename T >
inline
object_id
identify
(
    T &  obj
)
{
    return std::make_pair( static_cast<void *>(&obj), &(typeid( obj )) );
}

// Compare two object IDs
bool
object_id_compare::operator ()
(
    object_id const &  a,
    object_id const &  b
) const
{
    std::less<void *>  vp_cmp;
    if ( vp_cmp(a.first, b.first) )
    {
        return true;
    }
    else if ( vp_cmp(b.first, a.first) )
    {
        return false;
    }
    else
    {
        // object pointers are equal, compare the types
        if ( a.second == b.second )
        {
            return false;
        }
        else if ( !a.second )
        {
            return true;   // NULL preceeds anything else
        }
        else if ( !b.second )
        {
            return false;  // NULL preceeds anything else
        }
        else
        {
            return a.second->before( *b.second ) != 0;
        }
    }
}

// Let an object register its existence
void
object_registrar::register_object_imp
(
    object_id  obj
)
{
    if ( db_.count(obj) <= 0 )
    {
        db_.insert( obj );

        #if CONTROL_EXTRA_PRINTING
        std::cout << "Registered " << obj << '.' << std::endl;
        #endif
    }
    else
    {
        overeager_.push_back( obj );

        #if CONTROL_EXTRA_PRINTING
        std::cout << "Attempted to register a non-existant " << obj
         << '.' << std::endl;
        #endif
    }
}

// Let an object register its use of another object
void
object_registrar::register_use_imp
(
    object_id  owner,
    object_id  owned
)
{
    if ( db_.count(owned) > 0 )
    {
        // We don't care to record usage registrations
    }
    else
    {
        defrauders_in_.push_back( std::make_pair(owner, owned) );

        #if CONTROL_EXTRA_PRINTING
        std::cout << "Attempted to own a non-existant " << owned
         << " by " << owner << '.' << std::endl;
        #endif
    }
}

// Let an object un-register its use of another object
void
object_registrar::unregister_use_imp
(
    object_id  owner,
    object_id  owned
)
{
    if ( db_.count(owned) > 0 )
    {
        // We don't care to record usage un-registrations
    }
    else
    {
        defrauders_out_.push_back( std::make_pair(owner, owned) );

        #if CONTROL_EXTRA_PRINTING
        std::cout << "Attempted to disown a non-existant " << owned
         << " by " << owner << '.' << std::endl;
        #endif
    }
}

// Let an object un-register its existence
void
object_registrar::unregister_object_imp
(
    object_id  obj
)
{
    set_type::iterator const  i = db_.find( obj );

    if ( i != db_.end() )
    {
        db_.erase( i );

        #if CONTROL_EXTRA_PRINTING
        std::cout << "Unregistered " << obj << '.' << std::endl;
        #endif
    }
    else
    {
        overkilled_.push_back( obj );

        #if CONTROL_EXTRA_PRINTING
        std::cout << "Attempted to unregister a non-existant " << obj
         << '.' << std::endl;
        #endif
    }
}

// Macros to abstract the registration of objects
#ifndef BOOST_NO_MEMBER_TEMPLATES
#define PRIVATE_REGISTER_BIRTH(o)     obj_reg.register_object( (o) )
#define PRIVATE_REGISTER_DEATH(o)     obj_reg.unregister_object( (o) )
#define PRIVATE_REGISTER_USE(o, w)    obj_reg.register_use( (o), (w) )
#define PRIVATE_UNREGISTER_USE(o, w)  obj_reg.unregister_use( (o), (w) )
#else
#define PRIVATE_REGISTER_BIRTH(o)     obj_reg.register_object_imp( \
 identify((o)) )
#define PRIVATE_REGISTER_DEATH(o)     obj_reg.unregister_object_imp( \
 identify((o)) )
#define PRIVATE_REGISTER_USE(o, w)    obj_reg.register_use_imp( identify((o)), \
 identify((w)) )
#define PRIVATE_UNREGISTER_USE(o, w)  obj_reg.unregister_use_imp( \
 identify((o)), identify((w)) )
#endif

// Create a base_or_member, with arguments to simulate member initializations
base_or_member::base_or_member
(
    int     x,  // = 1
    double  y   // = -0.25
)
{
    PRIVATE_REGISTER_BIRTH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy x-factor is " << x << " and my y-factor is " << y
     << '.' << std::endl;
    #endif
}

// Destroy a base_or_member
inline
base_or_member::~base_or_member
(
)
{
    PRIVATE_REGISTER_DEATH( *this );
}

// Create a base_class, registering any objects used
base_class::base_class
(
    base_or_member &  x,
    base_or_member *  y,  // = 0
    base_or_member *  z   // = 0
)
    : x_( &x ), y_( y ), z_( z )
{
    PRIVATE_REGISTER_BIRTH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy x-factor is " << x_;
    #endif

    PRIVATE_REGISTER_USE( *this, *x_ );

    if ( y_ )
    {
        #if CONTROL_EXTRA_PRINTING
        std::cout << ", my y-factor is " << y_;
        #endif

        PRIVATE_REGISTER_USE( *this, *y_ );
    }

    if ( z_ )
    {
        #if CONTROL_EXTRA_PRINTING
        std::cout << ", my z-factor is " << z_;
        #endif

        PRIVATE_REGISTER_USE( *this, *z_ );
    }

    #if CONTROL_EXTRA_PRINTING
    std::cout << '.' << std::endl;
    #endif
}

// Destroy a base_class, unregistering the objects it uses
base_class::~base_class
(
)
{
    PRIVATE_REGISTER_DEATH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy x-factor was " << x_;
    #endif

    PRIVATE_UNREGISTER_USE( *this, *x_ );

    if ( y_ )
    {
        #if CONTROL_EXTRA_PRINTING
        std::cout << ", my y-factor was " << y_;
        #endif

        PRIVATE_UNREGISTER_USE( *this, *y_ );
    }

    if ( z_ )
    {
        #if CONTROL_EXTRA_PRINTING
        std::cout << ", my z-factor was " << z_;
        #endif

        PRIVATE_UNREGISTER_USE( *this, *z_ );
    }

    #if CONTROL_EXTRA_PRINTING
    std::cout << '.' << std::endl;
    #endif
}

// Create a bad_class, noting the improper construction order
bad_class::bad_class
(
)
    : x_( -7, 16.75 ), base_class( x_ )  // this order doesn't matter
{
    PRIVATE_REGISTER_BIRTH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy factor is at " << &x_
     << " and my base is at " << static_cast<base_class *>(this) << '.'
     << std::endl;
    #endif
}

// Destroy a bad_class, noting the improper destruction order
bad_class::~bad_class
(
)
{
    PRIVATE_REGISTER_DEATH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy factor was at " << &x_
     << " and my base was at " << static_cast<base_class *>(this)
     << '.' << std::endl;
    #endif
}

// Create a good_class_1, noting the proper construction order
good_class_1::good_class_1
(
)
    : pbase_type( 8 ), base_type( member )
{
    PRIVATE_REGISTER_BIRTH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy factor is at " << &member
     << " and my base is at " << static_cast<base_class *>(this) << '.'
     << std::endl;
    #endif
}

// Destroy a good_class_1, noting the proper destruction order
good_class_1::~good_class_1
(
)
{
    PRIVATE_REGISTER_DEATH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy factor was at " << &member
     << " and my base was at " << static_cast<base_class *>(this)
     << '.' << std::endl;
    #endif
}

// Create a good_class_2, noting the proper construction order
good_class_2::good_class_2
(
)
    : pbase_type0(), pbase_type1(-16, 0.125), pbase_type2(2, -3)
    , base_type( pbase_type1::member, &this->pbase_type0::member,
       &this->pbase_type2::member )
{
    PRIVATE_REGISTER_BIRTH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy factors are at " << &this->pbase_type0::member
     << ", " << &this->pbase_type1::member << ", "
     << &this->pbase_type2::member << ", and my base is at "
     << static_cast<base_class *>(this) << '.' << std::endl;
    #endif
}

// Destroy a good_class_2, noting the proper destruction order
good_class_2::~good_class_2
(
)
{
    PRIVATE_REGISTER_DEATH( *this );

    #if CONTROL_EXTRA_PRINTING
    std::cout << "\tMy factors were at " << &this->pbase_type0::member
     << ", " << &this->pbase_type1::member << ", "
     << &this->pbase_type2::member << ", and my base was at "
     << static_cast<base_class *>(this) << '.' << std::endl;
    #endif
}
