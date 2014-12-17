//  Boost rational.hpp header file  ------------------------------------------//

//  (C) Copyright Paul Moore 1999. Permission to copy, use, modify, sell and
//  distribute this software is granted provided this copyright notice appears
//  in all copies. This software is provided "as is" without express or
//  implied warranty, and with no claim as to its suitability for any purpose.

// boostinspect:nolicense (don't complain about the lack of a Boost license)
// (Paul Moore hasn't been in contact for years, so there's no way to change the
// license.)

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
//  05 Nov 06  Change rational_cast to not depend on division between different
//             types (Daryle Walker)
//  04 Nov 06  Off-load GCD and LCM to Boost.Math; add some invariant checks;
//             add std::numeric_limits<> requirement to help GCD (Daryle Walker)
//  31 Oct 06  Recoded both operator< to use round-to-negative-infinity
//             divisions; the rational-value version now uses continued fraction
//             expansion to avoid overflows, for bug #798357 (Daryle Walker)
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
#include <ios>                   // for std::noskipws
#include <stdexcept>             // for std::domain_error
#include <string>                // for std::string implicit constructor
#include <boost/operators.hpp>   // for boost::addable etc
#include <cstdlib>               // for std::abs
#include <boost/call_traits.hpp> // for boost::call_traits
#include <boost/config.hpp>      // for BOOST_NO_STDC_NAMESPACE, BOOST_MSVC
#include <boost/detail/workaround.hpp> // for BOOST_WORKAROUND
#include <boost/assert.hpp>      // for BOOST_ASSERT
#include <boost/math/common_factor_rt.hpp>  // for boost::math::gcd, lcm
#include <limits>                // for std::numeric_limits
#include <boost/static_assert.hpp>  // for BOOST_STATIC_ASSERT

// Control whether depreciated GCD and LCM functions are included (default: yes)
#ifndef BOOST_CONTROL_RATIONAL_HAS_GCD
#define BOOST_CONTROL_RATIONAL_HAS_GCD  1
#endif

namespace boost {

#if BOOST_CONTROL_RATIONAL_HAS_GCD
template <typename IntType>
IntType gcd(IntType n, IntType m)
{
    // Defer to the version in Boost.Math
    return math::gcd( n, m );
}

template <typename IntType>
IntType lcm(IntType n, IntType m)
{
    // Defer to the version in Boost.Math
    return math::lcm( n, m );
}
#endif  // BOOST_CONTROL_RATIONAL_HAS_GCD

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
    // Class-wide pre-conditions
    BOOST_STATIC_ASSERT( ::std::numeric_limits<IntType>::is_specialized );

    // Helper types
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
    bool test_invariant() const;
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

    IntType g = math::gcd(den, r_den);
    den /= g;  // = b1 from the calculations above
    num = num * (r_den / g) + r_num * den;
    g = math::gcd(num, g);
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
    IntType g = math::gcd(den, r_den);
    den /= g;
    num = num * (r_den / g) - r_num * den;
    g = math::gcd(num, g);
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
    IntType gcd1 = math::gcd(num, r_den);
    IntType gcd2 = math::gcd(r_num, den);
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
    IntType gcd1 = math::gcd(num, r_num);
    IntType gcd2 = math::gcd(r_den, den);
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
    int_type const  zero( 0 );

    // This should really be a class-wide invariant.  The reason for these
    // checks is that for 2's complement systems, INT_MIN has no corresponding
    // positive, so negating it during normalization keeps it INT_MIN, which
    // is bad for later calculations that assume a positive denominator.
    BOOST_ASSERT( this->den > zero );
    BOOST_ASSERT( r.den > zero );

    // Determine relative order by expanding each value to its simple continued
    // fraction representation using the Euclidian GCD algorithm.
    struct { int_type  n, d, q, r; }
     ts = { this->num, this->den, static_cast<int_type>(this->num / this->den),
     static_cast<int_type>(this->num % this->den) },
     rs = { r.num, r.den, static_cast<int_type>(r.num / r.den),
     static_cast<int_type>(r.num % r.den) };
    unsigned  reverse = 0u;

    // Normalize negative moduli by repeatedly adding the (positive) denominator
    // and decrementing the quotient.  Later cycles should have all positive
    // values, so this only has to be done for the first cycle.  (The rules of
    // C++ require a nonnegative quotient & remainder for a nonnegative dividend
    // & positive divisor.)
    while ( ts.r < zero )  { ts.r += ts.d; --ts.q; }
    while ( rs.r < zero )  { rs.r += rs.d; --rs.q; }

    // Loop through and compare each variable's continued-fraction components
    while ( true )
    {
        // The quotients of the current cycle are the continued-fraction
        // components.  Comparing two c.f. is comparing their sequences,
        // stopping at the first difference.
        if ( ts.q != rs.q )
        {
            // Since reciprocation changes the relative order of two variables,
            // and c.f. use reciprocals, the less/greater-than test reverses
            // after each index.  (Start w/ non-reversed @ whole-number place.)
            return reverse ? ts.q > rs.q : ts.q < rs.q;
        }

        // Prepare the next cycle
        reverse ^= 1u;

        if ( (ts.r == zero) || (rs.r == zero) )
        {
            // At least one variable's c.f. expansion has ended
            break;
        }

        ts.n = ts.d;         ts.d = ts.r;
        ts.q = ts.n / ts.d;  ts.r = ts.n % ts.d;
        rs.n = rs.d;         rs.d = rs.r;
        rs.q = rs.n / rs.d;  rs.r = rs.n % rs.d;
    }

    // Compare infinity-valued components for otherwise equal sequences
    if ( ts.r == rs.r )
    {
        // Both remainders are zero, so the next (and subsequent) c.f.
        // components for both sequences are infinity.  Therefore, the sequences
        // and their corresponding values are equal.
        return false;
    }
    else
    {
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4800)
#endif
        // Exactly one of the remainders is zero, so all following c.f.
        // components of that variable are infinity, while the other variable
        // has a finite next c.f. component.  So that other variable has the
        // lesser value (modulo the reversal flag!).
        return ( ts.r != zero ) != static_cast<bool>( reverse );
#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
    }
}

template <typename IntType>
bool rational<IntType>::operator< (param_type i) const
{
    // Avoid repeated construction
    int_type const  zero( 0 );

    // Break value into mixed-fraction form, w/ always-nonnegative remainder
    BOOST_ASSERT( this->den > zero );
    int_type  q = this->num / this->den, r = this->num % this->den;
    while ( r < zero )  { r += this->den; --q; }

    // Compare with just the quotient, since the remainder always bumps the
    // value up.  [Since q = floor(n/d), and if n/d < i then q < i, if n/d == i
    // then q == i, if n/d == i + r/d then q == i, and if n/d >= i + 1 then
    // q >= i + 1 > i; therefore n/d < i iff q < i.]
    return q < i;
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

// Invariant check
template <typename IntType>
inline bool rational<IntType>::test_invariant() const
{
    return ( this->den > int_type(0) ) && ( math::gcd(this->num, this->den) ==
     int_type(1) );
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

    IntType g = math::gcd(num, den);

    num /= g;
    den /= g;

    // Ensure that the denominator is positive
    if (den < zero) {
        num = -num;
        den = -den;
    }

    BOOST_ASSERT( this->test_invariant() );
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
    return static_cast<T>(src.numerator())/static_cast<T>(src.denominator());
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

