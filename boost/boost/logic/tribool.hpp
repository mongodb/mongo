// Three-state boolean logic library

// Copyright Douglas Gregor 2002-2004. Use, modification and
// distribution is subject to the Boost Software License, Version
// 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)


// For more information, see http://www.boost.org
#ifndef BOOST_LOGIC_TRIBOOL_HPP
#define BOOST_LOGIC_TRIBOOL_HPP

#include <boost/logic/tribool_fwd.hpp>
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(_MSC_VER, >= 1200)
#  pragma once
#endif

namespace boost { namespace logic {

/// INTERNAL ONLY
namespace detail {
/**
 * INTERNAL ONLY
 *
 * \brief A type used only to uniquely identify the 'indeterminate'
 * function/keyword.
 */
struct indeterminate_t
{
#if BOOST_WORKAROUND(__BORLANDC__, < 0x0600)
  char dummy_; // BCB would use 8 bytes by default
#endif
};

} // end namespace detail

/**
 * INTERNAL ONLY
 * The type of the 'indeterminate' keyword. This has the same type as the
 * function 'indeterminate' so that we can recognize when the keyword is
 * used.
 */
typedef bool (*indeterminate_keyword_t)(tribool, detail::indeterminate_t);

/**
 * \brief Keyword and test function for the indeterminate tribool value
 *
 * The \c indeterminate function has a dual role. It's first role is
 * as a unary function that tells whether the tribool value is in the
 * "indeterminate" state. It's second role is as a keyword
 * representing the indeterminate (just like "true" and "false"
 * represent the true and false states). If you do not like the name
 * "indeterminate", and would prefer to use a different name, see the
 * macro \c BOOST_TRIBOOL_THIRD_STATE.
 *
 * \returns <tt>x.value == tribool::indeterminate_value</tt>
 * \throws nothrow
 */
inline bool
indeterminate(tribool x,
              detail::indeterminate_t dummy = detail::indeterminate_t());

/**
 * \brief A 3-state boolean type.
 *
 * 3-state boolean values are either true, false, or
 * indeterminate.
 */
class tribool
{
private:
  /// INTERNAL ONLY
  struct dummy {
    void nonnull() {};
  };

  typedef void (dummy::*safe_bool)();

public:
  /**
   * Construct a new 3-state boolean value with the value 'false'.
   *
   * \throws nothrow
   */
  tribool() : value(false_value) {}

  /**
   * Construct a new 3-state boolean value with the given boolean
   * value, which may be \c true or \c false.
   *
   * \throws nothrow
   */
  tribool(bool value) : value(value? true_value : false_value) {}

  /**
   * Construct a new 3-state boolean value with an indeterminate value.
   *
   * \throws nothrow
   */
  tribool(indeterminate_keyword_t) : value(indeterminate_value) {}

  /**
   * Use a 3-state boolean in a boolean context. Will evaluate true in a
   * boolean context only when the 3-state boolean is definitely true.
   *
   * \returns true if the 3-state boolean is true, false otherwise
   * \throws nothrow
   */
  operator safe_bool() const
  {
    return value == true_value? &dummy::nonnull : 0;
  }

  /**
   * The actual stored value in this 3-state boolean, which may be false, true,
   * or indeterminate.
   */
  enum value_t { false_value, true_value, indeterminate_value } value;
};

// Check if the given tribool has an indeterminate value. Also doubles as a
// keyword for the 'indeterminate' value
inline bool indeterminate(tribool x, detail::indeterminate_t)
{
  return x.value == tribool::indeterminate_value;
}

/** @defgroup logical Logical operations
 */
//@{
/**
 * \brief Computes the logical negation of a tribool
 *
 * \returns the logical negation of the tribool, according to the
 * table:
 *  <table border=1>
 *    <tr>
 *      <th><center><code>!</code></center></th>
 *      <th/>
 *    </tr>
 *    <tr>
 *      <th><center>false</center></th>
 *      <td><center>true</center></td>
 *    </tr>
 *    <tr>
 *      <th><center>true</center></th>
 *      <td><center>false</center></td>
 *    </tr>
 *    <tr>
 *      <th><center>indeterminate</center></th>
 *      <td><center>indeterminate</center></td>
 *    </tr>
 *  </table>
 * \throws nothrow
 */
inline tribool operator!(tribool x)
{
  return x.value == tribool::false_value? tribool(true)
        :x.value == tribool::true_value? tribool(false)
        :tribool(indeterminate);
}

/**
 * \brief Computes the logical conjuction of two tribools
 *
 * \returns the result of logically ANDing the two tribool values,
 * according to the following table:
 *       <table border=1>
 *           <tr>
 *             <th><center><code>&amp;&amp;</code></center></th>
 *             <th><center>false</center></th>
 *             <th><center>true</center></th>
 *             <th><center>indeterminate</center></th>
 *           </tr>
 *           <tr>
 *             <th><center>false</center></th>
 *             <td><center>false</center></td>
 *             <td><center>false</center></td>
 *             <td><center>false</center></td>
 *           </tr>
 *           <tr>
 *             <th><center>true</center></th>
 *             <td><center>false</center></td>
 *             <td><center>true</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *           <tr>
 *             <th><center>indeterminate</center></th>
 *             <td><center>false</center></td>
 *             <td><center>indeterminate</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *       </table>
 * \throws nothrow
 */
inline tribool operator&&(tribool x, tribool y)
{
  if (static_cast<bool>(!x) || static_cast<bool>(!y))
    return false;
  else if (static_cast<bool>(x) && static_cast<bool>(y))
    return true;
  else
    return indeterminate;
}

/**
 * \overload
 */
inline tribool operator&&(tribool x, bool y)
{ return y? x : tribool(false); }

/**
 * \overload
 */
inline tribool operator&&(bool x, tribool y)
{ return x? y : tribool(false); }

/**
 * \overload
 */
inline tribool operator&&(indeterminate_keyword_t, tribool x)
{ return !x? tribool(false) : tribool(indeterminate); }

/**
 * \overload
 */
inline tribool operator&&(tribool x, indeterminate_keyword_t)
{ return !x? tribool(false) : tribool(indeterminate); }

/**
 * \brief Computes the logical disjunction of two tribools
 *
 * \returns the result of logically ORing the two tribool values,
 * according to the following table:
 *       <table border=1>
 *           <tr>
 *             <th><center><code>||</code></center></th>
 *             <th><center>false</center></th>
 *             <th><center>true</center></th>
 *             <th><center>indeterminate</center></th>
 *           </tr>
 *           <tr>
 *             <th><center>false</center></th>
 *             <td><center>false</center></td>
 *             <td><center>true</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *           <tr>
 *             <th><center>true</center></th>
 *             <td><center>true</center></td>
 *             <td><center>true</center></td>
 *             <td><center>true</center></td>
 *           </tr>
 *           <tr>
 *             <th><center>indeterminate</center></th>
 *             <td><center>indeterminate</center></td>
 *             <td><center>true</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *       </table>
 *  \throws nothrow
 */
inline tribool operator||(tribool x, tribool y)
{
  if (static_cast<bool>(!x) && static_cast<bool>(!y))
    return false;
  else if (static_cast<bool>(x) || static_cast<bool>(y))
    return true;
  else
    return indeterminate;
}

/**
 * \overload
 */
inline tribool operator||(tribool x, bool y)
{ return y? tribool(true) : x; }

/**
 * \overload
 */
inline tribool operator||(bool x, tribool y)
{ return x? tribool(true) : y; }

/**
 * \overload
 */
inline tribool operator||(indeterminate_keyword_t, tribool x)
{ return x? tribool(true) : tribool(indeterminate); }

/**
 * \overload
 */
inline tribool operator||(tribool x, indeterminate_keyword_t)
{ return x? tribool(true) : tribool(indeterminate); }
//@}

/**
 * \brief Compare tribools for equality
 *
 * \returns the result of comparing two tribool values, according to
 * the following table:
 *       <table border=1>
 *          <tr>
 *            <th><center><code>==</code></center></th>
 *            <th><center>false</center></th>
 *            <th><center>true</center></th>
 *            <th><center>indeterminate</center></th>
 *          </tr>
 *          <tr>
 *            <th><center>false</center></th>
 *            <td><center>true</center></td>
 *            <td><center>false</center></td>
 *            <td><center>indeterminate</center></td>
 *          </tr>
 *          <tr>
 *            <th><center>true</center></th>
 *            <td><center>false</center></td>
 *            <td><center>true</center></td>
 *            <td><center>indeterminate</center></td>
 *          </tr>
 *          <tr>
 *            <th><center>indeterminate</center></th>
 *            <td><center>indeterminate</center></td>
 *            <td><center>indeterminate</center></td>
 *            <td><center>indeterminate</center></td>
 *          </tr>
 *      </table>
 * \throws nothrow
 */
inline tribool operator==(tribool x, tribool y)
{
  if (indeterminate(x) || indeterminate(y))
    return indeterminate;
  else
    return x && y || !x && !y;
}

/**
 * \overload
 */
inline tribool operator==(tribool x, bool y) { return x == tribool(y); }

/**
 * \overload
 */
inline tribool operator==(bool x, tribool y) { return tribool(x) == y; }

/**
 * \overload
 */
inline tribool operator==(indeterminate_keyword_t, tribool x)
{ return tribool(indeterminate) == x; }

/**
 * \overload
 */
inline tribool operator==(tribool x, indeterminate_keyword_t)
{ return tribool(indeterminate) == x; }

/**
 * \brief Compare tribools for inequality
 *
 * \returns the result of comparing two tribool values for inequality,
 * according to the following table:
 *       <table border=1>
 *           <tr>
 *             <th><center><code>!=</code></center></th>
 *             <th><center>false</center></th>
 *             <th><center>true</center></th>
 *             <th><center>indeterminate</center></th>
 *           </tr>
 *           <tr>
 *             <th><center>false</center></th>
 *             <td><center>false</center></td>
 *             <td><center>true</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *           <tr>
 *             <th><center>true</center></th>
 *             <td><center>true</center></td>
 *             <td><center>false</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *           <tr>
 *             <th><center>indeterminate</center></th>
 *             <td><center>indeterminate</center></td>
 *             <td><center>indeterminate</center></td>
 *             <td><center>indeterminate</center></td>
 *           </tr>
 *       </table>
 * \throws nothrow
 */
inline tribool operator!=(tribool x, tribool y)
{
  if (indeterminate(x) || indeterminate(y))
    return indeterminate;
  else
    return !(x && y || !x && !y);
}

/**
 * \overload
 */
inline tribool operator!=(tribool x, bool y) { return x != tribool(y); }

/**
 * \overload
 */
inline tribool operator!=(bool x, tribool y) { return tribool(x) != y; }

/**
 * \overload
 */
inline tribool operator!=(indeterminate_keyword_t, tribool x)
{ return tribool(indeterminate) != x; }

/**
 * \overload
 */
inline tribool operator!=(tribool x, indeterminate_keyword_t)
{ return x != tribool(indeterminate); }

} } // end namespace boost::logic

// Pull tribool and indeterminate into namespace "boost"
namespace boost {
  using logic::tribool;
  using logic::indeterminate;
}

/**
 * \brief Declare a new name for the third state of a tribool
 *
 * Use this macro to declare a new name for the third state of a
 * tribool. This state can have any number of new names (in addition
 * to \c indeterminate), all of which will be equivalent. The new name will be
 * placed in the namespace in which the macro is expanded.
 *
 * Example:
 *   BOOST_TRIBOOL_THIRD_STATE(true_or_false)
 *
 *   tribool x(true_or_false);
 *   // potentially set x
 *   if (true_or_false(x)) {
 *     // don't know what x is
 *   }
 */
#define BOOST_TRIBOOL_THIRD_STATE(Name)                                 \
inline bool                                                             \
Name(boost::logic::tribool x,                                           \
     boost::logic::detail::indeterminate_t dummy =                      \
       boost::logic::detail::indeterminate_t())                         \
{ return x.value == boost::logic::tribool::indeterminate_value; }

#endif // BOOST_LOGIC_TRIBOOL_HPP

