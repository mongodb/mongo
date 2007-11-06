//  Boost rational.hpp header file  ------------------------------------------//

//  (C) Copyright Paul Moore 1999. Permission to copy, use, modify, sell and
//  distribute this software is granted provided this copyright notice appears
//  in all copies. This software is provided "as is" without express or
//  implied warranty, and with no claim as to its suitability for any purpose.

//  See http://www.boost.org/libs/rational for documentation.

//  Credits:
//  Thanks to the boost mailing list in general for useful comments.
//  Particular contributions included:
//    Andrew D Jewell, for reminding me to take care to avoid overflow
//    Ed Brey, for many comments, including picking up on some dreadful typos
//    Stephen Silver contributed the test suite and comments on user-defined
//    IntType
//    Nickolay Mladenov, for the implementation of operator+=

//  Revision History
//  20 Oct 06  Fix operator bool_type for CW 8.3 (Joaquín M López Muñoz)
//  18 Oct 06  Use EXPLICIT_TEMPLATE_TYPE helper macros from Boost.Config
//             (Joaquín M López Muñoz)
//  27 Dec 05  Add Boolean conversion operator (Daryle Walker)
//  28 Sep 02  Use _left versions of operators from operators.hpp
//  05 Jul 01  Recode gcd(), avoiding std::swap (Helmut Zeisel)
//  03 Mar 01  Workarounds for Intel C++ 5.0 (David Abrahams)
//  05 Feb 01  Update operator>> to tighten up input syntax
//  05 Feb 01  Final tidy up of gcd code prior to the new release
//  27 Jan 01  Recode abs() without relying on abs(IntType)
//  21 Jan 01  Include Nickolay Mladenov's operator+= algorithm,
//             tidy up a number of areas, use newer features of operators.hpp
//             (reduces space overhead to zero), add operator!,
//             introduce explicit mixed-mode arithmetic operations
//  12 Jan 01  Include fixes to handle a user-defined IntType better
//  19 Nov 00  Throw on divide by zero in operator /= (John (EBo) David)
//  23 Jun 00  Incorporate changes from Mark Rodgers for Borland C++
//  22 Jun 00  Change _MSC_VER to BOOST_MSVC so other compilers are not
//             affected (Beman Dawes)
//   6 Mar 00  Fix operator-= normalization, #include <string> (Jens Maurer)
//  14 Dec 99  Modifications based on comments from the boost list
//  09 Dec 99  Initial Version (Paul Moore)

#ifndef BOOST_RATIONAL_HPP
#define BOOST_RATIONAL_HPP

#include <iostream>              // for std::istream and std::ostream
#include <iomanip>               // for std::noskipws
#include <stdexcept>             // for std::domain_error
#include <string>                // for std::string implicit constructor
#include <boost/operators.hpp>   // for boost::addable etc
#include <cstdlib>               // for std::abs
#include <boost/call_traits.hpp> // for boost::call_traits
#include <boost/config.hpp>      // for BOOST_NO_STDC_NAMESPACE, BOOST_MSVC
#include <boost/detail/workaround.hpp> // for BOOST_WORKAROUND

namespace boost {

// Note: We use n and m as temporaries in this function, so there is no value
// in using const IntType& as we would only need to make a copy anyway...
template <typename IntType>
IntType gcd(IntType n, IntType m)
{
    // Avoid repeated construction
    IntType zero(0);

    // This is abs() - given the existence of broken compilers with Koenig
    // lookup issues and other problems, I code this explicitly. (Remember,
    // IntType may be a user-defined type).
    if (n < zero)
        n = -n;
    if (m < zero)
        m = -m;

    // As n and m are now positive, we can be sure that %= returns a
    // positive value (the standard guarantees this for built-in types,
    // and we require it of user-defined types).
    for(;;) {
      if(m == zero)
        return n;
      n %= m;
      if(n == zero)
        return m;
      m %= n;
    }
}

template <typename IntType>
IntType lcm(IntType n, IntType m)
{
    // Avoid repeated construction
    IntType zero(0);

    if (n == zero || m == zero)
        return zero;

    n /= gcd(n, m);
    n *= m;

    if (n < zero)
        n = -n;
    return n;
}

class bad_rational : public std::domain_error
{
public:
    explicit bad_rational() : std::domain_error("bad rational: zero denominator") {}
};

template <typename IntType>
class rational;

template <typename IntType>
rational<IntType> abs(const rational<IntType>& r);

template <typename IntType>
class rational :
    less_than_comparable < rational<IntType>,
    equality_comparable < rational<IntType>,
    less_than_comparable2 < rational<IntType>, IntType,
    equality_comparable2 < rational<IntType>, IntType,
    addable < rational<IntType>,
    subtractable < rational<IntType>,
    multipliable < rational<IntType>,
    dividable < rational<IntType>,
    addable2 < rational<IntType>, IntType,
    subtractable2 < rational<IntType>, IntType,
    subtractable2_left < rational<IntType>, IntType,
    multipliable2 < rational<IntType>, IntType,
    dividable2 < rational<IntType>, IntType,
    dividable2_left < rational<IntType>, IntType,
    incrementable < rational<IntType>,
    decrementable < rational<IntType>
    > > > > > > > > > > > > > > > >
{
    typedef typename boost::call_traits<IntType>::param_type param_type;

    struct helper { IntType parts[2]; };
    typedef IntType (helper::* bool_type)[2];

public:
    typedef IntType int_type;
    rational() : num(0), den(1) {}
    rational(param_type n) : num(n), den(1) {}
    rational(param_type n, param_type d) : num(n), den(d) { normalize(); }

    // Default copy constructor and assignment are fine

    // Add assignment from IntType
    rational& operator=(param_type n) { return assign(n, 1); }

    // Assign in place
    rational& assign(param_type n, param_type d);

    // Access to representation
    IntType numerator() const { return num; }
    IntType denominator() const { return den; }

    // Arithmetic assignment operators
    rational& operator+= (const rational& r);
    rational& operator-= (const rational& r);
    rational& operator*= (const rational& r);
    rational& operator/= (const rational& r);

    rational& operator+= (param_type i);
    rational& operator-= (param_type i);
    rational& operator*= (param_type i);
    rational& operator/= (param_type i);

    // Increment and decrement
    const rational& operator++();
    const rational& operator--();

    // Operator not
    bool operator!() const { return !num; }

    // Boolean conversion
    
#if BOOST_WORKAROUND(__MWERKS__,<=0x3003)
    // The "ISO C++ Template Parser" option in CW 8.3 chokes on the
    // following, hence we selectively disable that option for the
    // offending memfun.
#pragma parse_mfunc_templ off
#endif

    operator bool_type() const { return operator !() ? 0 : &helper::parts; }

#if BOOST_WORKAROUND(__MWERKS__,<=0x3003)
#pragma parse_mfunc_templ reset
#endif

    // Comparison operators
    bool operator< (const rational& r) const;
    bool operator== (const rational& r) const;

    bool operator< (param_type i) const;
    bool operator> (param_type i) const;
    bool operator== (param_type i) const;

private:
    // Implementation - numerator and denominator (normalized).
    // Other possibilities - separate whole-part, or sign, fields?
    IntType num;
    IntType den;

    // Representation note: Fractions are kept in normalized form at all
    // times. normalized form is defined as gcd(num,den) == 1 and den > 0.
    // In particular, note that the implementation of abs() below relies
    // on den always being positive.
    void normalize();
};

// Assign in place
template <typename IntType>
inline rational<IntType>& rational<IntType>::assign(param_type n, param_type d)
{
    num = n;
    den = d;
    normalize();
    return *this;
}

// Unary plus and minus
template <typename IntType>
inline rational<IntType> operator+ (const rational<IntType>& r)
{
    return r;
}

template <typename IntType>
inline rational<IntType> operator- (const rational<IntType>& r)
{
    return rational<IntType>(-r.numerator(), r.denominator());
}

// Arithmetic assignment operators
template <typename IntType>
rational<IntType>& rational<IntType>::operator+= (const rational<IntType>& r)
{
    // This calculation avoids overflow, and minimises the number of expensive
    // calculations. Thanks to Nickolay Mladenov for this algorithm.
    //
    // Proof:
    // We have to compute a/b + c/d, where gcd(a,b)=1 and gcd(b,c)=1.
    // Let g = gcd(b,d), and b = b1*g, d=d1*g. Then gcd(b1,d1)=1
    //
    // The result is (a*d1 + c*b1) / (b1*d1*g).
    // Now we have to normalize this ratio.
    // Let's assume h | gcd((a*d1 + c*b1), (b1*d1*g)), and h > 1
    // If h | b1 then gcd(h,d1)=1 and hence h|(a*d1+c*b1) => h|a.
    // But since gcd(a,b1)=1 we have h=1.
    // Similarly h|d1 leads to h=1.
    // So we have that h | gcd((a*d1 + c*b1) , (b1*d1*g)) => h|g
    // Finally we have gcd((a*d1 + c*b1), (b1*d1*g)) = gcd((a*d1 + c*b1), g)
    // Which proves that instead of normalizing the result, it is better to
    // divide num and den by gcd((a*d1 + c*b1), g)

    // Protect against self-modification
    IntType r_num = r.num;
    IntType r_den = r.den;

    IntType g = gcd(den, r_den);
    den /= g;  // = b1 from the calculations above
    num = num * (r_den / g) + r_num * den;
    g = gcd(num, g);
    num /= g;
    den *= r_den/g;

    return *this;
}

template <typename IntType>
rational<IntType>& rational<IntType>::operator-= (const rational<IntType>& r)
{
    // Protect against self-modification
    IntType r_num = r.num;
    IntType r_den = r.den;

    // This calculation avoids overflow, and minimises the number of expensive
    // calculations. It corresponds exactly to the += case above
    IntType g = gcd(den, r_den);
    den /= g;
    num = num * (r_den / g) - r_num * den;
    g = gcd(num, g);
    num /= g;
    den *= r_den/g;

    return *this;
}

template <typename IntType>
rational<IntType>& rational<IntType>::operator*= (const rational<IntType>& r)
{
    // Protect against self-modification
    IntType r_num = r.num;
    IntType r_den = r.den;

    // Avoid overflow and preserve normalization
    IntType gcd1 = gcd<IntType>(num, r_den);
    IntType gcd2 = gcd<IntType>(r_num, den);
    num = (num/gcd1) * (r_num/gcd2);
    den = (den/gcd2) * (r_den/gcd1);
    return *this;
}

template <typename IntType>
rational<IntType>& rational<IntType>::operator/= (const rational<IntType>& r)
{
    // Protect against self-modification
    IntType r_num = r.num;
    IntType r_den = r.den;

    // Avoid repeated construction
    IntType zero(0);

    // Trap division by zero
    if (r_num == zero)
        throw bad_rational();
    if (num == zero)
        return *this;

    // Avoid overflow and preserve normalization
    IntType gcd1 = gcd<IntType>(num, r_num);
    IntType gcd2 = gcd<IntType>(r_den, den);
    num = (num/gcd1) * (r_den/gcd2);
    den = (den/gcd2) * (r_num/gcd1);

    if (den < zero) {
        num = -num;
        den = -den;
    }
    return *this;
}

// Mixed-mode operators
template <typename IntType>
inline rational<IntType>&
rational<IntType>::operator+= (param_type i)
{
    return operator+= (rational<IntType>(i));
}

template <typename IntType>
inline rational<IntType>&
rational<IntType>::operator-= (param_type i)
{
    return operator-= (rational<IntType>(i));
}

template <typename IntType>
inline rational<IntType>&
rational<IntType>::operator*= (param_type i)
{
    return operator*= (rational<IntType>(i));
}

template <typename IntType>
inline rational<IntType>&
rational<IntType>::operator/= (param_type i)
{
    return operator/= (rational<IntType>(i));
}

// Increment and decrement
template <typename IntType>
inline const rational<IntType>& rational<IntType>::operator++()
{
    // This can never denormalise the fraction
    num += den;
    return *this;
}

template <typename IntType>
inline const rational<IntType>& rational<IntType>::operator--()
{
    // This can never denormalise the fraction
    num -= den;
    return *this;
}

// Comparison operators
template <typename IntType>
bool rational<IntType>::operator< (const rational<IntType>& r) const
{
    // Avoid repeated construction
    IntType zero(0);

    // If the two values have different signs, we don't need to do the
    // expensive calculations below. We take advantage here of the fact
    // that the denominator is always positive.
    if (num < zero && r.num >= zero) // -ve < +ve
        return true;
    if (num >= zero && r.num <= zero) // +ve or zero is not < -ve or zero
        return false;

    // Avoid overflow
    IntType gcd1 = gcd<IntType>(num, r.num);
    IntType gcd2 = gcd<IntType>(r.den, den);
    return (num/gcd1) * (r.den/gcd2) < (den/gcd2) * (r.num/gcd1);
}

template <typename IntType>
bool rational<IntType>::operator< (param_type i) const
{
    // Avoid repeated construction
    IntType zero(0);

    // If the two values have different signs, we don't need to do the
    // expensive calculations below. We take advantage here of the fact
    // that the denominator is always positive.
    if (num < zero && i >= zero) // -ve < +ve
        return true;
    if (num >= zero && i <= zero) // +ve or zero is not < -ve or zero
        return false;

    // Now, use the fact that n/d truncates towards zero as long as n and d
    // are both positive.
    // Divide instead of multiplying to avoid overflow issues. Of course,
    // division may be slower, but accuracy is more important than speed...
    if (num > zero)
        return (num/den) < i;
    else
        return -i < (-num/den);
}

template <typename IntType>
bool rational<IntType>::operator> (param_type i) const
{
    // Trap equality first
    if (num == i && den == IntType(1))
        return false;

    // Otherwise, we can use operator<
    return !operator<(i);
}

template <typename IntType>
inline bool rational<IntType>::operator== (const rational<IntType>& r) const
{
    return ((num == r.num) && (den == r.den));
}

template <typename IntType>
inline bool rational<IntType>::operator== (param_type i) const
{
    return ((den == IntType(1)) && (num == i));
}

// Normalisation
template <typename IntType>
void rational<IntType>::normalize()
{
    // Avoid repeated construction
    IntType zero(0);

    if (den == zero)
        throw bad_rational();

    // Handle the case of zero separately, to avoid division by zero
    if (num == zero) {
        den = IntType(1);
        return;
    }

    IntType g = gcd<IntType>(num, den);

    num /= g;
    den /= g;

    // Ensure that the denominator is positive
    if (den < zero) {
        num = -num;
        den = -den;
    }
}

namespace detail {

    // A utility class to reset the format flags for an istream at end
    // of scope, even in case of exceptions
    struct resetter {
        resetter(std::istream& is) : is_(is), f_(is.flags()) {}
        ~resetter() { is_.flags(f_); }
        std::istream& is_;
        std::istream::fmtflags f_;      // old GNU c++ lib has no ios_base
    };

}

// Input and output
template <typename IntType>
std::istream& operator>> (std::istream& is, rational<IntType>& r)
{
    IntType n = IntType(0), d = IntType(1);
    char c = 0;
    detail::resetter sentry(is);

    is >> n;
    c = is.get();

    if (c != '/')
        is.clear(std::istream::badbit);  // old GNU c++ lib has no ios_base

#if !defined(__GNUC__) || (defined(__GNUC__) && (__GNUC__ >= 3)) || defined __SGI_STL_PORT
    is >> std::noskipws;
#else
    is.unsetf(ios::skipws); // compiles, but seems to have no effect.
#endif
    is >> d;

    if (is)
        r.assign(n, d);

    return is;
}

// Add manipulators for output format?
template <typename IntType>
std::ostream& operator<< (std::ostream& os, const rational<IntType>& r)
{
    os << r.numerator() << '/' << r.denominator();
    return os;
}

// Type conversion
template <typename T, typename IntType>
inline T rational_cast(
    const rational<IntType>& src BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(T))
{
    return static_cast<T>(src.numerator())/src.denominator();
}

// Do not use any abs() defined on IntType - it isn't worth it, given the
// difficulties involved (Koenig lookup required, there may not *be* an abs()
// defined, etc etc).
template <typename IntType>
inline rational<IntType> abs(const rational<IntType>& r)
{
    if (r.numerator() >= IntType(0))
        return r;

    return rational<IntType>(-r.numerator(), r.denominator());
}

} // namespace boost

#endif  // BOOST_RATIONAL_HPP

