//  Demonstrate and test boost/operators.hpp  -------------------------------//

//  Copyright Beman Dawes 1999.  Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/utility for documentation.

//  Revision History
//  03 Apr 08 Added convertible_to_bool (Daniel Frey)
//  01 Oct 01 Added tests for "left" operators
//            and new grouped operators. (Helmut Zeisel)
//  20 May 01 Output progress messages.  Added tests for new operator
//            templates.  Updated random number generator.  Changed tests to
//            use Boost Test Tools library.  (Daryle Walker)
//  04 Jun 00 Added regression test for a bug I found (David Abrahams)
//  17 Jun 00 Fix for broken compilers (Aleksey Gurtovoy)
//  ?? ??? 00 Major update to randomly test all one- and two- argument forms by
//            wrapping integral types and comparing the results of operations
//            to the results for the raw types (David Abrahams)
//  12 Dec 99 Minor update, output confirmation message.
//  15 Nov 99 Initial version

#define BOOST_INCLUDE_MAIN

#include <boost/config.hpp>                      // for BOOST_MSVC
#include <boost/cstdlib.hpp>                     // for boost::exit_success
#include <boost/operators.hpp>                   // for the tested items
#include <boost/random/linear_congruential.hpp>  // for boost::minstd_rand
#include <boost/test/test_tools.hpp>             // for main

#include <iostream>  // for std::cout (std::endl indirectly)


namespace
{
    // avoiding a template version of true_value so as to not confuse VC++
    int true_value(int x) { return x; }
    long true_value(long x) { return x; }
    signed char true_value(signed char x) { return x; }
    short true_value(short x) { return x; }
    unsigned int true_value(unsigned int x) { return x; }
    unsigned long true_value(unsigned long x) { return x; }
    unsigned char true_value(unsigned char x) { return x; }
    unsigned short true_value(unsigned short x) { return x; }

    // verify the minimum requirements for some operators
    class convertible_to_bool
    {
    private:
        bool _value;

        typedef bool convertible_to_bool::*unspecified_bool_type;

        void operator!() const;

    public:
        convertible_to_bool( const bool value ) : _value( value ) {}

        operator unspecified_bool_type() const
          { return _value ? &convertible_to_bool::_value : 0; }
    };

    // The use of operators<> here tended to obscure
    // interactions with certain compiler bugs
    template <class T>
    class Wrapped1
        : boost::operators<Wrapped1<T> >
        , boost::shiftable<Wrapped1<T> >
    {
    public:
        explicit Wrapped1( T v = T() ) : _value(v) {}
        T value() const { return _value; }

        convertible_to_bool operator<(const Wrapped1& x) const
          { return _value < x._value; }
        convertible_to_bool operator==(const Wrapped1& x) const
          { return _value == x._value; }
        
        Wrapped1& operator+=(const Wrapped1& x)
          { _value += x._value; return *this; }
        Wrapped1& operator-=(const Wrapped1& x)
          { _value -= x._value; return *this; }
        Wrapped1& operator*=(const Wrapped1& x)
          { _value *= x._value; return *this; }
        Wrapped1& operator/=(const Wrapped1& x)
          { _value /= x._value; return *this; }
        Wrapped1& operator%=(const Wrapped1& x)
          { _value %= x._value; return *this; }
        Wrapped1& operator|=(const Wrapped1& x)
          { _value |= x._value; return *this; }
        Wrapped1& operator&=(const Wrapped1& x)
          { _value &= x._value; return *this; }
        Wrapped1& operator^=(const Wrapped1& x)
          { _value ^= x._value; return *this; }
        Wrapped1& operator<<=(const Wrapped1& x)
          { _value <<= x._value; return *this; }
        Wrapped1& operator>>=(const Wrapped1& x)
          { _value >>= x._value; return *this; }
        Wrapped1& operator++()               { ++_value; return *this; }
        Wrapped1& operator--()               { --_value; return *this; }
        
    private:
        T _value;
    };
    template <class T>
    T true_value(Wrapped1<T> x) { return x.value(); }    

    template <class T, class U>
    class Wrapped2
        : boost::operators<Wrapped2<T, U> >
        , boost::operators2<Wrapped2<T, U>, U>
        , boost::shiftable1<Wrapped2<T, U>
        , boost::shiftable2<Wrapped2<T, U>, U > >
    {
    public:
        explicit Wrapped2( T v = T() ) : _value(v) {}
        T value() const { return _value; }

        convertible_to_bool operator<(const Wrapped2& x) const
          { return _value < x._value; }
        convertible_to_bool operator==(const Wrapped2& x) const
          { return _value == x._value; }
        
        Wrapped2& operator+=(const Wrapped2& x)
          { _value += x._value; return *this; }
        Wrapped2& operator-=(const Wrapped2& x)
          { _value -= x._value; return *this; }
        Wrapped2& operator*=(const Wrapped2& x)
          { _value *= x._value; return *this; }
        Wrapped2& operator/=(const Wrapped2& x)
          { _value /= x._value; return *this; }
        Wrapped2& operator%=(const Wrapped2& x)
          { _value %= x._value; return *this; }
        Wrapped2& operator|=(const Wrapped2& x)
          { _value |= x._value; return *this; }
        Wrapped2& operator&=(const Wrapped2& x)
          { _value &= x._value; return *this; }
        Wrapped2& operator^=(const Wrapped2& x)
          { _value ^= x._value; return *this; }
        Wrapped2& operator<<=(const Wrapped2& x)
          { _value <<= x._value; return *this; }
        Wrapped2& operator>>=(const Wrapped2& x)
          { _value >>= x._value; return *this; }
        Wrapped2& operator++()                { ++_value; return *this; }
        Wrapped2& operator--()                { --_value; return *this; }
         
        convertible_to_bool operator<(U u) const
          { return _value < u; }
        convertible_to_bool operator>(U u) const
          { return _value > u; }
        convertible_to_bool operator==(U u) const
          { return _value == u; }

        Wrapped2& operator+=(U u) { _value += u; return *this; }
        Wrapped2& operator-=(U u) { _value -= u; return *this; }
        Wrapped2& operator*=(U u) { _value *= u; return *this; }
        Wrapped2& operator/=(U u) { _value /= u; return *this; }
        Wrapped2& operator%=(U u) { _value %= u; return *this; }
        Wrapped2& operator|=(U u) { _value |= u; return *this; }
        Wrapped2& operator&=(U u) { _value &= u; return *this; }
        Wrapped2& operator^=(U u) { _value ^= u; return *this; }
        Wrapped2& operator<<=(U u) { _value <<= u; return *this; }
        Wrapped2& operator>>=(U u) { _value >>= u; return *this; }

    private:
        T _value;
    };
    template <class T, class U>
    T true_value(Wrapped2<T,U> x) { return x.value(); }
    
    template <class T>
    class Wrapped3
        : boost::equivalent<Wrapped3<T> >
        , boost::partially_ordered<Wrapped3<T> >
        , boost::equality_comparable<Wrapped3<T> >
    {
    public:
        explicit Wrapped3( T v = T() ) : _value(v) {}
        T value() const { return _value; }

        convertible_to_bool operator<(const Wrapped3& x) const
          { return _value < x._value; }
        
    private:
        T _value;
    };
    template <class T>
    T true_value(Wrapped3<T> x) { return x.value(); }    

    template <class T, class U>
    class Wrapped4
        : boost::equality_comparable1<Wrapped4<T, U>
        , boost::equivalent1<Wrapped4<T, U>
        , boost::partially_ordered1<Wrapped4<T, U> > > >
        , boost::partially_ordered2<Wrapped4<T, U>, U
        , boost::equivalent2<Wrapped4<T, U>, U
        , boost::equality_comparable2<Wrapped4<T, U>, U> > >
    {
    public:
        explicit Wrapped4( T v = T() ) : _value(v) {}
        T value() const { return _value; }

        convertible_to_bool operator<(const Wrapped4& x) const
          { return _value < x._value; }
         
        convertible_to_bool operator<(U u) const
          { return _value < u; }
        convertible_to_bool operator>(U u) const
          { return _value > u; }

    private:
        T _value;
    };
    template <class T, class U>
    T true_value(Wrapped4<T,U> x) { return x.value(); }
    
    // U must be convertible to T
    template <class T, class U>
    class Wrapped5
        : boost::ordered_field_operators2<Wrapped5<T, U>, U>
        , boost::ordered_field_operators1<Wrapped5<T, U> >
    {
    public:
        explicit Wrapped5( T v = T() ) : _value(v) {}

        // Conversion from U to Wrapped5<T,U>
        Wrapped5(U u) : _value(u) {}

        T value() const { return _value; }

        convertible_to_bool operator<(const Wrapped5& x) const
          { return _value < x._value; }
        convertible_to_bool operator<(U u) const
          { return _value < u; }
        convertible_to_bool operator>(U u) const
          { return _value > u; }
        convertible_to_bool operator==(const Wrapped5& u) const
          { return _value == u._value; }
        convertible_to_bool operator==(U u) const
          { return _value == u; }

        Wrapped5& operator/=(const Wrapped5& u) { _value /= u._value; return *this;}
        Wrapped5& operator/=(U u) { _value /= u; return *this;}
        Wrapped5& operator*=(const Wrapped5& u) { _value *= u._value; return *this;}
        Wrapped5& operator*=(U u) { _value *= u; return *this;}
        Wrapped5& operator-=(const Wrapped5& u) { _value -= u._value; return *this;}
        Wrapped5& operator-=(U u) { _value -= u; return *this;}
        Wrapped5& operator+=(const Wrapped5& u) { _value += u._value; return *this;}
        Wrapped5& operator+=(U u) { _value += u; return *this;}

    private:
        T _value;
    };
    template <class T, class U>
    T true_value(Wrapped5<T,U> x) { return x.value(); }
    
    // U must be convertible to T
    template <class T, class U>
    class Wrapped6
        : boost::ordered_euclidean_ring_operators2<Wrapped6<T, U>, U>
        , boost::ordered_euclidean_ring_operators1<Wrapped6<T, U> >
    {
    public:
        explicit Wrapped6( T v = T() ) : _value(v) {}

        // Conversion from U to Wrapped6<T,U>
        Wrapped6(U u) : _value(u) {}

        T value() const { return _value; }

        convertible_to_bool operator<(const Wrapped6& x) const
          { return _value < x._value; }
        convertible_to_bool operator<(U u) const
          { return _value < u; }
        convertible_to_bool operator>(U u) const
          { return _value > u; }
        convertible_to_bool operator==(const Wrapped6& u) const
          { return _value == u._value; }
        convertible_to_bool operator==(U u) const
          { return _value == u; }

        Wrapped6& operator%=(const Wrapped6& u) { _value %= u._value; return *this;}
        Wrapped6& operator%=(U u) { _value %= u; return *this;}
        Wrapped6& operator/=(const Wrapped6& u) { _value /= u._value; return *this;}
        Wrapped6& operator/=(U u) { _value /= u; return *this;}
        Wrapped6& operator*=(const Wrapped6& u) { _value *= u._value; return *this;}
        Wrapped6& operator*=(U u) { _value *= u; return *this;}
        Wrapped6& operator-=(const Wrapped6& u) { _value -= u._value; return *this;}
        Wrapped6& operator-=(U u) { _value -= u; return *this;}
        Wrapped6& operator+=(const Wrapped6& u) { _value += u._value; return *this;}
        Wrapped6& operator+=(U u) { _value += u; return *this;}

    private:
        T _value;
    };
    template <class T, class U>
    T true_value(Wrapped6<T,U> x) { return x.value(); }
    
    //  MyInt uses only the single template-argument form of all_operators<>
    typedef Wrapped1<int> MyInt;

    typedef Wrapped2<long, long> MyLong;

    typedef Wrapped3<signed char> MyChar;

    typedef Wrapped4<short, short> MyShort;

    typedef Wrapped5<double, int> MyDoubleInt;

    typedef Wrapped6<long, int> MyLongInt;

    template <class X1, class Y1, class X2, class Y2>
    void sanity_check(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        BOOST_CHECK( true_value(y1) == true_value(y2) );
        BOOST_CHECK( true_value(x1) == true_value(x2) );
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_less_than_comparable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        BOOST_CHECK( static_cast<bool>(x1 < y1) == static_cast<bool>(x2 < y2) );
        BOOST_CHECK( static_cast<bool>(x1 <= y1) == static_cast<bool>(x2 <= y2) );
        BOOST_CHECK( static_cast<bool>(x1 >= y1) == static_cast<bool>(x2 >= y2) );
        BOOST_CHECK( static_cast<bool>(x1 > y1) == static_cast<bool>(x2 > y2) );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_less_than_comparable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_less_than_comparable_aux( x1, y1, x2, y2 );
        test_less_than_comparable_aux( y1, x1, y2, x2 );
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_equality_comparable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        BOOST_CHECK( static_cast<bool>(x1 == y1) == static_cast<bool>(x2 == y2) );
        BOOST_CHECK( static_cast<bool>(x1 != y1) == static_cast<bool>(x2 != y2) );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_equality_comparable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_equality_comparable_aux( x1, y1, x2, y2 );
        test_equality_comparable_aux( y1, x1, y2, x2 );
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_multipliable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        BOOST_CHECK( (x1 * y1).value() == (x2 * y2) );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_multipliable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_multipliable_aux( x1, y1, x2, y2 );
        test_multipliable_aux( y1, x1, y2, x2 );
    }

  template <class A, class B>
  void test_value_equality(A a, B b)
  {
      BOOST_CHECK(a.value() == b);
  }
  
#define TEST_OP_R(op) test_value_equality(x1 op y1, x2 op y2)
#define TEST_OP_L(op) test_value_equality(y1 op x1, y2 op x2)
  
    template <class X1, class Y1, class X2, class Y2>
    void test_addable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        TEST_OP_R(+);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_addable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_addable_aux( x1, y1, x2, y2 );
        test_addable_aux( y1, x1, y2, x2 );
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_subtractable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        TEST_OP_R(-);
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_subtractable_left(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        TEST_OP_L(-);
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_dividable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        if ( y2 != 0 )
            TEST_OP_R(/);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_dividable_left(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        if ( x2 != 0 )
            TEST_OP_L(/);
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_modable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        if ( y2 != 0 )
            TEST_OP_R(%);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_modable_left(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        if ( x2 != 0 )
            TEST_OP_L(%);
    }

    template <class X1, class Y1, class X2, class Y2>
    void test_xorable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        TEST_OP_R(^);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_xorable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_xorable_aux( x1, y1, x2, y2 );
        test_xorable_aux( y1, x1, y2, x2 );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_andable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        TEST_OP_R(&);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_andable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_andable_aux( x1, y1, x2, y2 );
        test_andable_aux( y1, x1, y2, x2 );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_orable_aux(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        TEST_OP_R(|);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_orable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        test_orable_aux( x1, y1, x2, y2 );
        test_orable_aux( y1, x1, y2, x2 );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_left_shiftable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        TEST_OP_R(<<);
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_right_shiftable(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        sanity_check( x1, y1, x2, y2 );
        TEST_OP_R(>>);
    }
    
    template <class X1, class X2>
    void test_incrementable(X1 x1, X2 x2)
    {
        sanity_check( x1, x1, x2, x2 );
        BOOST_CHECK( (x1++).value() == x2++ );
        BOOST_CHECK( x1.value() == x2 );
    }
    
    template <class X1, class X2>
    void test_decrementable(X1 x1, X2 x2)
    {
        sanity_check( x1, x1, x2, x2 );
        BOOST_CHECK( (x1--).value() == x2-- );
        BOOST_CHECK( x1.value() == x2 );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_all(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        test_less_than_comparable( x1, y1, x2, y2 );
        test_equality_comparable( x1, y1, x2, y2 );
        test_multipliable( x1, y1, x2, y2 );
        test_addable( x1, y1, x2, y2 );
        test_subtractable( x1, y1, x2, y2 );
        test_dividable( x1, y1, x2, y2 );
        test_modable( x1, y1, x2, y2 );
        test_xorable( x1, y1, x2, y2 );
        test_andable( x1, y1, x2, y2 );
        test_orable( x1, y1, x2, y2 );
        test_left_shiftable( x1, y1, x2, y2 );
        test_right_shiftable( x1, y1, x2, y2 );
        test_incrementable( x1, x2 );
        test_decrementable( x1, x2 );
    }
    
    template <class X1, class Y1, class X2, class Y2>
    void test_left(X1 x1, Y1 y1, X2 x2, Y2 y2)
    {
        test_subtractable_left( x1, y1, x2, y2 );
        test_dividable_left( x1, y1, x2, y2 );
        test_modable_left( x1, y1, x2, y2 );
     }

    template <class Big, class Small>
    struct tester
    {
        void operator()(boost::minstd_rand& randomizer) const
        {
            Big    b1 = Big( randomizer() );
            Big    b2 = Big( randomizer() );
            Small  s = Small( randomizer() );
            
            test_all( Wrapped1<Big>(b1), Wrapped1<Big>(b2), b1, b2 );
            test_all( Wrapped2<Big, Small>(b1), s, b1, s );
        }
    };

    template <class Big, class Small>
    struct tester_left
    {
        void operator()(boost::minstd_rand& randomizer) const
        {
            Big    b1 = Big( randomizer() );
            Small  s = Small( randomizer() );
            
            test_left( Wrapped6<Big, Small>(b1), s, b1, s );
         }
    };

    // added as a regression test. We had a bug which this uncovered.
    struct Point
        : boost::addable<Point
        , boost::subtractable<Point> >
    {
        Point( int h, int v ) : h(h), v(v) {}
        Point() :h(0), v(0) {}
        const Point& operator+=( const Point& rhs )
            { h += rhs.h; v += rhs.v; return *this; }
        const Point& operator-=( const Point& rhs )
            { h -= rhs.h; v -= rhs.v; return *this; }

        int h;
        int v;
    };

} // unnamed namespace


// workaround for MSVC bug; for some reasons the compiler doesn't instantiate
// inherited operator templates at the moment it must, so the following
// explicit instantiations force it to do that.

#if defined(BOOST_MSVC) && (_MSC_VER < 1300)
template Wrapped1<int>;
template Wrapped1<long>;
template Wrapped1<unsigned int>;
template Wrapped1<unsigned long>;

template Wrapped2<int, int>;
template Wrapped2<int, signed char>;
template Wrapped2<long, signed char>;
template Wrapped2<long, int>;
template Wrapped2<long, long>;
template Wrapped2<unsigned int, unsigned int>;
template Wrapped2<unsigned int, unsigned char>;
template Wrapped2<unsigned long, unsigned int>;
template Wrapped2<unsigned long, unsigned char>;
template Wrapped2<unsigned long, unsigned long>;

template Wrapped6<long, int>;
template Wrapped6<long, signed char>;
template Wrapped6<int, signed char>;
template Wrapped6<unsigned long, unsigned int>;
template Wrapped6<unsigned long, unsigned char>;
template Wrapped6<unsigned int, unsigned char>;
#endif

#define PRIVATE_EXPR_TEST(e, t)  BOOST_CHECK( ((e), (t)) )

int
test_main( int , char * [] )
{
    using std::cout;
    using std::endl;

    // Regression test.
    Point x;
    x = x + Point(3, 4);
    x = x - Point(3, 4);
    
    cout << "Created point, and operated on it." << endl;
    
    for (int n = 0; n < 1000; ++n)  // was 10,000 but took too long (Beman)
    {
        boost::minstd_rand r;
        tester<long, int>()(r);
        tester<long, signed char>()(r);
        tester<long, long>()(r);
        tester<int, int>()(r);
        tester<int, signed char>()(r);
        
        tester<unsigned long, unsigned int>()(r);
        tester<unsigned long, unsigned char>()(r);
        tester<unsigned long, unsigned long>()(r);
        tester<unsigned int, unsigned int>()(r);
        tester<unsigned int, unsigned char>()(r);

        tester_left<long, int>()(r);
        tester_left<long, signed char>()(r);
        tester_left<int, signed char>()(r);

        tester_left<unsigned long, unsigned int>()(r);
        tester_left<unsigned long, unsigned char>()(r);
        tester_left<unsigned int, unsigned char>()(r);
    }
    
    cout << "Did random tester loop." << endl;

    MyInt i1(1);
    MyInt i2(2);
    MyInt i;

    BOOST_CHECK( i1.value() == 1 );
    BOOST_CHECK( i2.value() == 2 );
    BOOST_CHECK( i.value() == 0 );

    cout << "Created MyInt objects.\n";

    PRIVATE_EXPR_TEST( (i = i2), (i.value() == 2) );

    BOOST_CHECK( static_cast<bool>(i2 == i) );
    BOOST_CHECK( static_cast<bool>(i1 != i2) );
    BOOST_CHECK( static_cast<bool>(i1 <  i2) );
    BOOST_CHECK( static_cast<bool>(i1 <= i2) );
    BOOST_CHECK( static_cast<bool>(i <= i2) );
    BOOST_CHECK( static_cast<bool>(i2 >  i1) );
    BOOST_CHECK( static_cast<bool>(i2 >= i1) );
    BOOST_CHECK( static_cast<bool>(i2 >= i) );

    PRIVATE_EXPR_TEST( (i = i1 + i2), (i.value() == 3) );
    PRIVATE_EXPR_TEST( (i = i + i2), (i.value() == 5) );
    PRIVATE_EXPR_TEST( (i = i - i1), (i.value() == 4) );
    PRIVATE_EXPR_TEST( (i = i * i2), (i.value() == 8) );
    PRIVATE_EXPR_TEST( (i = i / i2), (i.value() == 4) );
    PRIVATE_EXPR_TEST( (i = i % ( i - i1 )), (i.value() == 1) );
    PRIVATE_EXPR_TEST( (i = i2 + i2), (i.value() == 4) );
    PRIVATE_EXPR_TEST( (i = i1 | i2 | i), (i.value() == 7) );
    PRIVATE_EXPR_TEST( (i = i & i2), (i.value() == 2) );
    PRIVATE_EXPR_TEST( (i = i + i1), (i.value() == 3) );
    PRIVATE_EXPR_TEST( (i = i ^ i1), (i.value() == 2) );
    PRIVATE_EXPR_TEST( (i = ( i + i1 ) * ( i2 | i1 )), (i.value() == 9) );

    PRIVATE_EXPR_TEST( (i = i1 << i2), (i.value() == 4) );
    PRIVATE_EXPR_TEST( (i = i2 >> i1), (i.value() == 1) );

    cout << "Performed tests on MyInt objects.\n";

    MyLong j1(1);
    MyLong j2(2);
    MyLong j;

    BOOST_CHECK( j1.value() == 1 );
    BOOST_CHECK( j2.value() == 2 );
    BOOST_CHECK( j.value() == 0 );

    cout << "Created MyLong objects.\n";

    PRIVATE_EXPR_TEST( (j = j2), (j.value() == 2) );
    
    BOOST_CHECK( static_cast<bool>(j2 == j) );
    BOOST_CHECK( static_cast<bool>(2 == j) );
    BOOST_CHECK( static_cast<bool>(j2 == 2) );
    BOOST_CHECK( static_cast<bool>(j == j2) );
    BOOST_CHECK( static_cast<bool>(j1 != j2) );
    BOOST_CHECK( static_cast<bool>(j1 != 2) );
    BOOST_CHECK( static_cast<bool>(1 != j2) );
    BOOST_CHECK( static_cast<bool>(j1 <  j2) );
    BOOST_CHECK( static_cast<bool>(1 <  j2) );
    BOOST_CHECK( static_cast<bool>(j1 <  2) );
    BOOST_CHECK( static_cast<bool>(j1 <= j2) );
    BOOST_CHECK( static_cast<bool>(1 <= j2) );
    BOOST_CHECK( static_cast<bool>(j1 <= j) );
    BOOST_CHECK( static_cast<bool>(j <= j2) );
    BOOST_CHECK( static_cast<bool>(2 <= j2) );
    BOOST_CHECK( static_cast<bool>(j <= 2) );
    BOOST_CHECK( static_cast<bool>(j2 >  j1) );
    BOOST_CHECK( static_cast<bool>(2 >  j1) );
    BOOST_CHECK( static_cast<bool>(j2 >  1) );
    BOOST_CHECK( static_cast<bool>(j2 >= j1) );
    BOOST_CHECK( static_cast<bool>(2 >= j1) );
    BOOST_CHECK( static_cast<bool>(j2 >= 1) );
    BOOST_CHECK( static_cast<bool>(j2 >= j) );
    BOOST_CHECK( static_cast<bool>(2 >= j) );
    BOOST_CHECK( static_cast<bool>(j2 >= 2) );

    BOOST_CHECK( static_cast<bool>((j1 + 2) == 3) );
    BOOST_CHECK( static_cast<bool>((1 + j2) == 3) );
    PRIVATE_EXPR_TEST( (j = j1 + j2), (j.value() == 3) );
    
    BOOST_CHECK( static_cast<bool>((j + 2) == 5) );
    BOOST_CHECK( static_cast<bool>((3 + j2) == 5) );
    PRIVATE_EXPR_TEST( (j = j + j2), (j.value() == 5) );
    
    BOOST_CHECK( static_cast<bool>((j - 1) == 4) );
    PRIVATE_EXPR_TEST( (j = j - j1), (j.value() == 4) );
    
    BOOST_CHECK( static_cast<bool>((j * 2) == 8) );
    BOOST_CHECK( static_cast<bool>((4 * j2) == 8) );
    PRIVATE_EXPR_TEST( (j = j * j2), (j.value() == 8) );
    
    BOOST_CHECK( static_cast<bool>((j / 2) == 4) );
    PRIVATE_EXPR_TEST( (j = j / j2), (j.value() == 4) );
    
    BOOST_CHECK( static_cast<bool>((j % 3) == 1) );
    PRIVATE_EXPR_TEST( (j = j % ( j - j1 )), (j.value() == 1) );
    
    PRIVATE_EXPR_TEST( (j = j2 + j2), (j.value() == 4) );
    
    BOOST_CHECK( static_cast<bool>((1 | j2 | j) == 7) );
    BOOST_CHECK( static_cast<bool>((j1 | 2 | j) == 7) );
    BOOST_CHECK( static_cast<bool>((j1 | j2 | 4) == 7) );
    PRIVATE_EXPR_TEST( (j = j1 | j2 | j), (j.value() == 7) );
    
    BOOST_CHECK( static_cast<bool>((7 & j2) == 2) );
    BOOST_CHECK( static_cast<bool>((j & 2) == 2) );
    PRIVATE_EXPR_TEST( (j = j & j2), (j.value() == 2) );
    
    PRIVATE_EXPR_TEST( (j = j | j1), (j.value() == 3) );
    
    BOOST_CHECK( static_cast<bool>((3 ^ j1) == 2) );
    BOOST_CHECK( static_cast<bool>((j ^ 1) == 2) );
    PRIVATE_EXPR_TEST( (j = j ^ j1), (j.value() == 2) );
    
    PRIVATE_EXPR_TEST( (j = ( j + j1 ) * ( j2 | j1 )), (j.value() == 9) );

    BOOST_CHECK( static_cast<bool>((j1 << 2) == 4) );
    BOOST_CHECK( static_cast<bool>((j2 << 1) == 4) );
    PRIVATE_EXPR_TEST( (j = j1 << j2), (j.value() == 4) );

    BOOST_CHECK( static_cast<bool>((j >> 2) == 1) );
    BOOST_CHECK( static_cast<bool>((j2 >> 1) == 1) );
    PRIVATE_EXPR_TEST( (j = j2 >> j1), (j.value() == 1) );
    
    cout << "Performed tests on MyLong objects.\n";

    MyChar k1(1);
    MyChar k2(2);
    MyChar k;

    BOOST_CHECK( k1.value() == 1 );
    BOOST_CHECK( k2.value() == 2 );
    BOOST_CHECK( k.value() == 0 );

    cout << "Created MyChar objects.\n";

    PRIVATE_EXPR_TEST( (k = k2), (k.value() == 2) );

    BOOST_CHECK( static_cast<bool>(k2 == k) );
    BOOST_CHECK( static_cast<bool>(k1 != k2) );
    BOOST_CHECK( static_cast<bool>(k1 <  k2) );
    BOOST_CHECK( static_cast<bool>(k1 <= k2) );
    BOOST_CHECK( static_cast<bool>(k <= k2) );
    BOOST_CHECK( static_cast<bool>(k2 >  k1) );
    BOOST_CHECK( static_cast<bool>(k2 >= k1) );
    BOOST_CHECK( static_cast<bool>(k2 >= k) );
    
    cout << "Performed tests on MyChar objects.\n";

    MyShort l1(1);
    MyShort l2(2);
    MyShort l;

    BOOST_CHECK( l1.value() == 1 );
    BOOST_CHECK( l2.value() == 2 );
    BOOST_CHECK( l.value() == 0 );

    cout << "Created MyShort objects.\n";

    PRIVATE_EXPR_TEST( (l = l2), (l.value() == 2) );
    
    BOOST_CHECK( static_cast<bool>(l2 == l) );
    BOOST_CHECK( static_cast<bool>(2 == l) );
    BOOST_CHECK( static_cast<bool>(l2 == 2) );
    BOOST_CHECK( static_cast<bool>(l == l2) );
    BOOST_CHECK( static_cast<bool>(l1 != l2) );
    BOOST_CHECK( static_cast<bool>(l1 != 2) );
    BOOST_CHECK( static_cast<bool>(1 != l2) );
    BOOST_CHECK( static_cast<bool>(l1 <  l2) );
    BOOST_CHECK( static_cast<bool>(1 <  l2) );
    BOOST_CHECK( static_cast<bool>(l1 <  2) );
    BOOST_CHECK( static_cast<bool>(l1 <= l2) );
    BOOST_CHECK( static_cast<bool>(1 <= l2) );
    BOOST_CHECK( static_cast<bool>(l1 <= l) );
    BOOST_CHECK( static_cast<bool>(l <= l2) );
    BOOST_CHECK( static_cast<bool>(2 <= l2) );
    BOOST_CHECK( static_cast<bool>(l <= 2) );
    BOOST_CHECK( static_cast<bool>(l2 >  l1) );
    BOOST_CHECK( static_cast<bool>(2 >  l1) );
    BOOST_CHECK( static_cast<bool>(l2 >  1) );
    BOOST_CHECK( static_cast<bool>(l2 >= l1) );
    BOOST_CHECK( static_cast<bool>(2 >= l1) );
    BOOST_CHECK( static_cast<bool>(l2 >= 1) );
    BOOST_CHECK( static_cast<bool>(l2 >= l) );
    BOOST_CHECK( static_cast<bool>(2 >= l) );
    BOOST_CHECK( static_cast<bool>(l2 >= 2) );
    
    cout << "Performed tests on MyShort objects.\n";
    
    MyDoubleInt di1(1);
    MyDoubleInt di2(2.);
    MyDoubleInt half(0.5);
    MyDoubleInt di;
    MyDoubleInt tmp;

    BOOST_CHECK( di1.value() == 1 );
    BOOST_CHECK( di2.value() == 2 );
    BOOST_CHECK( di2.value() == 2 );
    BOOST_CHECK( di.value() == 0 );

    cout << "Created MyDoubleInt objects.\n";

    PRIVATE_EXPR_TEST( (di = di2), (di.value() == 2) );
    
    BOOST_CHECK( static_cast<bool>(di2 == di) );
    BOOST_CHECK( static_cast<bool>(2 == di) );
    BOOST_CHECK( static_cast<bool>(di == 2) );
    BOOST_CHECK( static_cast<bool>(di1 < di2) );
    BOOST_CHECK( static_cast<bool>(1 < di2) );
    BOOST_CHECK( static_cast<bool>(di1 <= di2) );
    BOOST_CHECK( static_cast<bool>(1 <= di2) );
    BOOST_CHECK( static_cast<bool>(di2 > di1) );
    BOOST_CHECK( static_cast<bool>(di2 > 1) );
    BOOST_CHECK( static_cast<bool>(di2 >= di1) );
    BOOST_CHECK( static_cast<bool>(di2 >= 1) );
    BOOST_CHECK( static_cast<bool>(di1 / di2 == half) );
    BOOST_CHECK( static_cast<bool>(di1 / 2 == half) );
    BOOST_CHECK( static_cast<bool>(1 / di2 == half) );
    PRIVATE_EXPR_TEST( (tmp=di1), static_cast<bool>((tmp/=2) == half) );
    PRIVATE_EXPR_TEST( (tmp=di1), static_cast<bool>((tmp/=di2) == half) );
    BOOST_CHECK( static_cast<bool>(di1 * di2 == di2) );
    BOOST_CHECK( static_cast<bool>(di1 * 2 == di2) );
    BOOST_CHECK( static_cast<bool>(1 * di2 == di2) );
    PRIVATE_EXPR_TEST( (tmp=di1), static_cast<bool>((tmp*=2) == di2) );
    PRIVATE_EXPR_TEST( (tmp=di1), static_cast<bool>((tmp*=di2) == di2) );
    BOOST_CHECK( static_cast<bool>(di2 - di1 == di1) );
    BOOST_CHECK( static_cast<bool>(di2 - 1 == di1) );
    BOOST_CHECK( static_cast<bool>(2 - di1 == di1) );
    PRIVATE_EXPR_TEST( (tmp=di2), static_cast<bool>((tmp-=1) == di1) );
    PRIVATE_EXPR_TEST( (tmp=di2), static_cast<bool>((tmp-=di1) == di1) );
    BOOST_CHECK( static_cast<bool>(di1 + di1 == di2) );
    BOOST_CHECK( static_cast<bool>(di1 + 1 == di2) );
    BOOST_CHECK( static_cast<bool>(1 + di1 == di2) );
    PRIVATE_EXPR_TEST( (tmp=di1), static_cast<bool>((tmp+=1) == di2) );
    PRIVATE_EXPR_TEST( (tmp=di1), static_cast<bool>((tmp+=di1) == di2) );

    cout << "Performed tests on MyDoubleInt objects.\n";

    MyLongInt li1(1);
    MyLongInt li2(2);
    MyLongInt li;
    MyLongInt tmp2;

    BOOST_CHECK( li1.value() == 1 );
    BOOST_CHECK( li2.value() == 2 );
    BOOST_CHECK( li.value() == 0 );

    cout << "Created MyLongInt objects.\n";

    PRIVATE_EXPR_TEST( (li = li2), (li.value() == 2) );
    
    BOOST_CHECK( static_cast<bool>(li2 == li) );
    BOOST_CHECK( static_cast<bool>(2 == li) );
    BOOST_CHECK( static_cast<bool>(li == 2) );
    BOOST_CHECK( static_cast<bool>(li1 < li2) );
    BOOST_CHECK( static_cast<bool>(1 < li2) );
    BOOST_CHECK( static_cast<bool>(li1 <= li2) );
    BOOST_CHECK( static_cast<bool>(1 <= li2) );
    BOOST_CHECK( static_cast<bool>(li2 > li1) );
    BOOST_CHECK( static_cast<bool>(li2 > 1) );
    BOOST_CHECK( static_cast<bool>(li2 >= li1) );
    BOOST_CHECK( static_cast<bool>(li2 >= 1) );
    BOOST_CHECK( static_cast<bool>(li1 % li2 == li1) );
    BOOST_CHECK( static_cast<bool>(li1 % 2 == li1) );
    BOOST_CHECK( static_cast<bool>(1 % li2 == li1) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2%=2) == li1) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2%=li2) == li1) );
    BOOST_CHECK( static_cast<bool>(li1 / li2 == 0) );
    BOOST_CHECK( static_cast<bool>(li1 / 2 == 0) );
    BOOST_CHECK( static_cast<bool>(1 / li2 == 0) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2/=2) == 0) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2/=li2) == 0) );
    BOOST_CHECK( static_cast<bool>(li1 * li2 == li2) );
    BOOST_CHECK( static_cast<bool>(li1 * 2 == li2) );
    BOOST_CHECK( static_cast<bool>(1 * li2 == li2) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2*=2) == li2) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2*=li2) == li2) );
    BOOST_CHECK( static_cast<bool>(li2 - li1 == li1) );
    BOOST_CHECK( static_cast<bool>(li2 - 1 == li1) );
    BOOST_CHECK( static_cast<bool>(2 - li1 == li1) );
    PRIVATE_EXPR_TEST( (tmp2=li2), static_cast<bool>((tmp2-=1) == li1) );
    PRIVATE_EXPR_TEST( (tmp2=li2), static_cast<bool>((tmp2-=li1) == li1) );
    BOOST_CHECK( static_cast<bool>(li1 + li1 == li2) );
    BOOST_CHECK( static_cast<bool>(li1 + 1 == li2) );
    BOOST_CHECK( static_cast<bool>(1 + li1 == li2) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2+=1) == li2) );
    PRIVATE_EXPR_TEST( (tmp2=li1), static_cast<bool>((tmp2+=li1) == li2) );

    cout << "Performed tests on MyLongInt objects.\n";

    return boost::exit_success;
}
