//  (C) Copyright John Maddock 2006.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_TOOLS_NEWTON_SOLVER_HPP
#define BOOST_MATH_TOOLS_NEWTON_SOLVER_HPP

#ifdef _MSC_VER
#pragma once
#endif
#include <boost/multiprecision/detail/number_base.hpp> // test for multiprecision types.
#include <boost/type_traits/is_complex.hpp> // test for complex types

#include <iostream>
#include <utility>
#include <boost/config/no_tr1/cmath.hpp>
#include <stdexcept>

#include <boost/math/tools/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>

#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable: 4512)
#endif
#include <boost/math/tools/tuple.hpp>
#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

#include <boost/math/special_functions/sign.hpp>
#include <boost/math/tools/toms748_solve.hpp>
#include <boost/math/policies/error_handling.hpp>

namespace boost{ namespace math{ namespace tools{

namespace detail{

namespace dummy{

   template<int n, class T>
   typename T::value_type get(const T&) BOOST_MATH_NOEXCEPT(T);
}

template <class Tuple, class T>
void unpack_tuple(const Tuple& t, T& a, T& b) BOOST_MATH_NOEXCEPT(T)
{
   using dummy::get;
   // Use ADL to find the right overload for get:
   a = get<0>(t);
   b = get<1>(t);
}
template <class Tuple, class T>
void unpack_tuple(const Tuple& t, T& a, T& b, T& c) BOOST_MATH_NOEXCEPT(T)
{
   using dummy::get;
   // Use ADL to find the right overload for get:
   a = get<0>(t);
   b = get<1>(t);
   c = get<2>(t);
}

template <class Tuple, class T>
inline void unpack_0(const Tuple& t, T& val) BOOST_MATH_NOEXCEPT(T)
{
   using dummy::get;
   // Rely on ADL to find the correct overload of get:
   val = get<0>(t);
}

template <class T, class U, class V>
inline void unpack_tuple(const std::pair<T, U>& p, V& a, V& b) BOOST_MATH_NOEXCEPT(T)
{
   a = p.first;
   b = p.second;
}
template <class T, class U, class V>
inline void unpack_0(const std::pair<T, U>& p, V& a) BOOST_MATH_NOEXCEPT(T)
{
   a = p.first;
}

template <class F, class T>
void handle_zero_derivative(F f,
                            T& last_f0,
                            const T& f0,
                            T& delta,
                            T& result,
                            T& guess,
                            const T& min,
                            const T& max) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   if(last_f0 == 0)
   {
      // this must be the first iteration, pretend that we had a
      // previous one at either min or max:
      if(result == min)
      {
         guess = max;
      }
      else
      {
         guess = min;
      }
      unpack_0(f(guess), last_f0);
      delta = guess - result;
   }
   if(sign(last_f0) * sign(f0) < 0)
   {
      // we've crossed over so move in opposite direction to last step:
      if(delta < 0)
      {
         delta = (result - min) / 2;
      }
      else
      {
         delta = (result - max) / 2;
      }
   }
   else
   {
      // move in same direction as last step:
      if(delta < 0)
      {
         delta = (result - max) / 2;
      }
      else
      {
         delta = (result - min) / 2;
      }
   }
}

} // namespace

template <class F, class T, class Tol, class Policy>
std::pair<T, T> bisect(F f, T min, T max, Tol tol, boost::uintmax_t& max_iter, const Policy& pol) BOOST_NOEXCEPT_IF(policies::is_noexcept_error_policy<Policy>::value && BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   T fmin = f(min);
   T fmax = f(max);
   if(fmin == 0)
   {
      max_iter = 2;
      return std::make_pair(min, min);
   }
   if(fmax == 0)
   {
      max_iter = 2;
      return std::make_pair(max, max);
   }

   //
   // Error checking:
   //
   static const char* function = "boost::math::tools::bisect<%1%>";
   if(min >= max)
   {
      return boost::math::detail::pair_from_single(policies::raise_evaluation_error(function,
         "Arguments in wrong order in boost::math::tools::bisect (first arg=%1%)", min, pol));
   }
   if(fmin * fmax >= 0)
   {
      return boost::math::detail::pair_from_single(policies::raise_evaluation_error(function,
         "No change of sign in boost::math::tools::bisect, either there is no root to find, or there are multiple roots in the interval (f(min) = %1%).", fmin, pol));
   }

   //
   // Three function invocations so far:
   //
   boost::uintmax_t count = max_iter;
   if(count < 3)
      count = 0;
   else
      count -= 3;

   while(count && (0 == tol(min, max)))
   {
      T mid = (min + max) / 2;
      T fmid = f(mid);
      if((mid == max) || (mid == min))
         break;
      if(fmid == 0)
      {
         min = max = mid;
         break;
      }
      else if(sign(fmid) * sign(fmin) < 0)
      {
         max = mid;
         fmax = fmid;
      }
      else
      {
         min = mid;
         fmin = fmid;
      }
      --count;
   }

   max_iter -= count;

#ifdef BOOST_MATH_INSTRUMENT
   std::cout << "Bisection iteration, final count = " << max_iter << std::endl;

   static boost::uintmax_t max_count = 0;
   if(max_iter > max_count)
   {
      max_count = max_iter;
      std::cout << "Maximum iterations: " << max_iter << std::endl;
   }
#endif

   return std::make_pair(min, max);
}

template <class F, class T, class Tol>
inline std::pair<T, T> bisect(F f, T min, T max, Tol tol, boost::uintmax_t& max_iter)  BOOST_NOEXCEPT_IF(policies::is_noexcept_error_policy<policies::policy<> >::value && BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   return bisect(f, min, max, tol, max_iter, policies::policy<>());
}

template <class F, class T, class Tol>
inline std::pair<T, T> bisect(F f, T min, T max, Tol tol) BOOST_NOEXCEPT_IF(policies::is_noexcept_error_policy<policies::policy<> >::value && BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   boost::uintmax_t m = (std::numeric_limits<boost::uintmax_t>::max)();
   return bisect(f, min, max, tol, m, policies::policy<>());
}


template <class F, class T>
T newton_raphson_iterate(F f, T guess, T min, T max, int digits, boost::uintmax_t& max_iter) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   BOOST_MATH_STD_USING

   T f0(0), f1, last_f0(0);
   T result = guess;

   T factor = static_cast<T>(ldexp(1.0, 1 - digits));
   T delta = tools::max_value<T>();
   T delta1 = tools::max_value<T>();
   T delta2 = tools::max_value<T>();

   boost::uintmax_t count(max_iter);

   do{
      last_f0 = f0;
      delta2 = delta1;
      delta1 = delta;
      detail::unpack_tuple(f(result), f0, f1);
      --count;
      if(0 == f0)
         break;
      if(f1 == 0)
      {
         // Oops zero derivative!!!
#ifdef BOOST_MATH_INSTRUMENT
         std::cout << "Newton iteration, zero derivative found" << std::endl;
#endif
         detail::handle_zero_derivative(f, last_f0, f0, delta, result, guess, min, max);
      }
      else
      {
         delta = f0 / f1;
      }
#ifdef BOOST_MATH_INSTRUMENT
      std::cout << "Newton iteration, delta = " << delta << std::endl;
#endif
      if(fabs(delta * 2) > fabs(delta2))
      {
         // last two steps haven't converged.
         T shift = (delta > 0) ? (result - min) / 2 : (result - max) / 2;
         if ((result != 0) && (fabs(shift) > fabs(result)))
         {
            delta = sign(delta) * result; // protect against huge jumps!
         }
         else
            delta = shift;
         // reset delta1/2 so we don't take this branch next time round:
         delta1 = 3 * delta;
         delta2 = 3 * delta;
      }
      guess = result;
      result -= delta;
      if(result <= min)
      {
         delta = 0.5F * (guess - min);
         result = guess - delta;
         if((result == min) || (result == max))
            break;
      }
      else if(result >= max)
      {
         delta = 0.5F * (guess - max);
         result = guess - delta;
         if((result == min) || (result == max))
            break;
      }
      // update brackets:
      if(delta > 0)
         max = guess;
      else
         min = guess;
   }while(count && (fabs(result * factor) < fabs(delta)));

   max_iter -= count;

#ifdef BOOST_MATH_INSTRUMENT
   std::cout << "Newton Raphson iteration, final count = " << max_iter << std::endl;

   static boost::uintmax_t max_count = 0;
   if(max_iter > max_count)
   {
      max_count = max_iter;
      std::cout << "Maximum iterations: " << max_iter << std::endl;
   }
#endif

   return result;
}

template <class F, class T>
inline T newton_raphson_iterate(F f, T guess, T min, T max, int digits) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   boost::uintmax_t m = (std::numeric_limits<boost::uintmax_t>::max)();
   return newton_raphson_iterate(f, guess, min, max, digits, m);
}

namespace detail{

   struct halley_step
   {
      template <class T>
      static T step(const T& /*x*/, const T& f0, const T& f1, const T& f2) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T))
      {
         using std::fabs;
         T denom = 2 * f0;
         T num = 2 * f1 - f0 * (f2 / f1);
         T delta;

         BOOST_MATH_INSTRUMENT_VARIABLE(denom);
         BOOST_MATH_INSTRUMENT_VARIABLE(num);

         if((fabs(num) < 1) && (fabs(denom) >= fabs(num) * tools::max_value<T>()))
         {
            // possible overflow, use Newton step:
            delta = f0 / f1;
         }
         else
            delta = denom / num;
         return delta;
      }
   };

   template <class Stepper, class F, class T>
   T second_order_root_finder(F f, T guess, T min, T max, int digits, boost::uintmax_t& max_iter) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
   {
      BOOST_MATH_STD_USING

         T f0(0), f1, f2;
      T result = guess;

      T factor = ldexp(static_cast<T>(1.0), 1 - digits);
      T delta = (std::max)(T(10000000 * guess), T(10000000));  // arbitarily large delta
      T last_f0 = 0;
      T delta1 = delta;
      T delta2 = delta;
      bool out_of_bounds_sentry = false;

#ifdef BOOST_MATH_INSTRUMENT
      std::cout << "Second order root iteration, limit = " << factor << std::endl;
#endif

      boost::uintmax_t count(max_iter);

      do{
         last_f0 = f0;
         delta2 = delta1;
         delta1 = delta;
         detail::unpack_tuple(f(result), f0, f1, f2);
         --count;

         BOOST_MATH_INSTRUMENT_VARIABLE(f0);
         BOOST_MATH_INSTRUMENT_VARIABLE(f1);
         BOOST_MATH_INSTRUMENT_VARIABLE(f2);

         if(0 == f0)
            break;
         if(f1 == 0)
         {
            // Oops zero derivative!!!
#ifdef BOOST_MATH_INSTRUMENT
            std::cout << "Second order root iteration, zero derivative found" << std::endl;
#endif
            detail::handle_zero_derivative(f, last_f0, f0, delta, result, guess, min, max);
         }
         else
         {
            if(f2 != 0)
            {
               delta = Stepper::step(result, f0, f1, f2);
               if(delta * f1 / f0 < 0)
               {
                  // Oh dear, we have a problem as Newton and Halley steps
                  // disagree about which way we should move.  Probably
                  // there is cancelation error in the calculation of the
                  // Halley step, or else the derivatives are so small
                  // that their values are basically trash.  We will move
                  // in the direction indicated by a Newton step, but
                  // by no more than twice the current guess value, otherwise
                  // we can jump way out of bounds if we're not careful.
                  // See https://svn.boost.org/trac/boost/ticket/8314.
                  delta = f0 / f1;
                  if(fabs(delta) > 2 * fabs(guess))
                     delta = (delta < 0 ? -1 : 1) * 2 * fabs(guess);
               }
            }
            else
               delta = f0 / f1;
         }
#ifdef BOOST_MATH_INSTRUMENT
         std::cout << "Second order root iteration, delta = " << delta << std::endl;
#endif
         T convergence = fabs(delta / delta2);
         if((convergence > 0.8) && (convergence < 2))
         {
            // last two steps haven't converged.
            delta = (delta > 0) ? (result - min) / 2 : (result - max) / 2;
            if ((result != 0) && (fabs(delta) > result))
               delta = sign(delta) * result; // protect against huge jumps!
            // reset delta2 so that this branch will *not* be taken on the
            // next iteration:
            delta2 = delta * 3;
            delta1 = delta * 3;
            BOOST_MATH_INSTRUMENT_VARIABLE(delta);
         }
         guess = result;
         result -= delta;
         BOOST_MATH_INSTRUMENT_VARIABLE(result);

         // check for out of bounds step:
         if(result < min)
         {
            T diff = ((fabs(min) < 1) && (fabs(result) > 1) && (tools::max_value<T>() / fabs(result) < fabs(min)))
               ? T(1000)
               : (fabs(min) < 1) && (fabs(tools::max_value<T>() * min) < fabs(result))
               ? ((min < 0) != (result < 0)) ? -tools::max_value<T>() : tools::max_value<T>() : T(result / min);
            if(fabs(diff) < 1)
               diff = 1 / diff;
            if(!out_of_bounds_sentry && (diff > 0) && (diff < 3))
            {
               // Only a small out of bounds step, lets assume that the result
               // is probably approximately at min:
               delta = 0.99f * (guess - min);
               result = guess - delta;
               out_of_bounds_sentry = true; // only take this branch once!
            }
            else
            {
               delta = (guess - min) / 2;
               result = guess - delta;
               if((result == min) || (result == max))
                  break;
            }
         }
         else if(result > max)
         {
            T diff = ((fabs(max) < 1) && (fabs(result) > 1) && (tools::max_value<T>() / fabs(result) < fabs(max))) ? T(1000) : T(result / max);
            if(fabs(diff) < 1)
               diff = 1 / diff;
            if(!out_of_bounds_sentry && (diff > 0) && (diff < 3))
            {
               // Only a small out of bounds step, lets assume that the result
               // is probably approximately at min:
               delta = 0.99f * (guess - max);
               result = guess - delta;
               out_of_bounds_sentry = true; // only take this branch once!
            }
            else
            {
               delta = (guess - max) / 2;
               result = guess - delta;
               if((result == min) || (result == max))
                  break;
            }
         }
         // update brackets:
         if(delta > 0)
            max = guess;
         else
            min = guess;
      } while(count && (fabs(result * factor) < fabs(delta)));

      max_iter -= count;

#ifdef BOOST_MATH_INSTRUMENT
      std::cout << "Second order root iteration, final count = " << max_iter << std::endl;
#endif

      return result;
   }

}

template <class F, class T>
T halley_iterate(F f, T guess, T min, T max, int digits, boost::uintmax_t& max_iter) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   return detail::second_order_root_finder<detail::halley_step>(f, guess, min, max, digits, max_iter);
}

template <class F, class T>
inline T halley_iterate(F f, T guess, T min, T max, int digits) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   boost::uintmax_t m = (std::numeric_limits<boost::uintmax_t>::max)();
   return halley_iterate(f, guess, min, max, digits, m);
}

namespace detail{

   struct schroder_stepper
   {
      template <class T>
      static T step(const T& x, const T& f0, const T& f1, const T& f2) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T))
      {
         using std::fabs;
         T ratio = f0 / f1;
         T delta;
         if((x != 0) && (fabs(ratio / x) < 0.1))
         {
            delta = ratio + (f2 / (2 * f1)) * ratio * ratio;
            // check second derivative doesn't over compensate:
            if(delta * ratio < 0)
               delta = ratio;
         }
         else
            delta = ratio;  // fall back to Newton iteration.
         return delta;
      }
   };

}

template <class F, class T>
T schroder_iterate(F f, T guess, T min, T max, int digits, boost::uintmax_t& max_iter) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   return detail::second_order_root_finder<detail::schroder_stepper>(f, guess, min, max, digits, max_iter);
}

template <class F, class T>
inline T schroder_iterate(F f, T guess, T min, T max, int digits) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   boost::uintmax_t m = (std::numeric_limits<boost::uintmax_t>::max)();
   return schroder_iterate(f, guess, min, max, digits, m);
}
//
// These two are the old spelling of this function, retained for backwards compatibity just in case:
//
template <class F, class T>
T schroeder_iterate(F f, T guess, T min, T max, int digits, boost::uintmax_t& max_iter) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   return detail::second_order_root_finder<detail::schroder_stepper>(f, guess, min, max, digits, max_iter);
}

template <class F, class T>
inline T schroeder_iterate(F f, T guess, T min, T max, int digits) BOOST_NOEXCEPT_IF(BOOST_MATH_IS_FLOAT(T) && noexcept(std::declval<F>()(std::declval<T>())))
{
   boost::uintmax_t m = (std::numeric_limits<boost::uintmax_t>::max)();
   return schroder_iterate(f, guess, min, max, digits, m);
}

#ifndef BOOST_NO_CXX11_AUTO_DECLARATIONS
/*
 * Why do we set the default maximum number of iterations to the number of digits in the type?
 * Because for double roots, the number of digits increases linearly with the number of iterations,
 * so this default should recover full precision even in this somewhat pathological case.
 * For isolated roots, the problem is so rapidly convergent that this doesn't matter at all.
 */
template<class Complex, class F>
Complex complex_newton(F g, Complex guess, int max_iterations=std::numeric_limits<typename Complex::value_type>::digits)
{
    typedef typename Complex::value_type Real;
    using std::norm;
    using std::abs;
    using std::max;
    // z0, z1, and z2 cannot be the same, in case we immediately need to resort to Muller's Method:
    Complex z0 = guess + Complex(1,0);
    Complex z1 = guess + Complex(0,1);
    Complex z2 = guess;

    do {
       auto pair = g(z2);
       if (norm(pair.second) == 0)
       {
           // Muller's method. Notation follows Numerical Recipes, 9.5.2:
           Complex q = (z2 - z1)/(z1 - z0);
           auto P0 = g(z0);
           auto P1 = g(z1);
           Complex qp1 = static_cast<Complex>(1)+q;
           Complex A = q*(pair.first - qp1*P1.first + q*P0.first);

           Complex B = (static_cast<Complex>(2)*q+static_cast<Complex>(1))*pair.first - qp1*qp1*P1.first +q*q*P0.first;
           Complex C = qp1*pair.first;
           Complex rad = sqrt(B*B - static_cast<Complex>(4)*A*C);
           Complex denom1 = B + rad;
           Complex denom2 = B - rad;
           Complex correction = (z1-z2)*static_cast<Complex>(2)*C;
           if (norm(denom1) > norm(denom2))
           {
               correction /= denom1;
           }
           else
           {
               correction /= denom2;
           }

           z0 = z1;
           z1 = z2;
           z2 = z2 + correction;
       }
       else
       {
           z0 = z1;
           z1 = z2;
           z2 = z2  - (pair.first/pair.second);
       }

       // See: https://math.stackexchange.com/questions/3017766/constructing-newton-iteration-converging-to-non-root
       // If f' is continuous, then convergence of x_n -> x* implies f(x*) = 0.
       // This condition approximates this convergence condition by requiring three consecutive iterates to be clustered.
       Real tol = max(abs(z2)*std::numeric_limits<Real>::epsilon(), std::numeric_limits<Real>::epsilon());
       bool real_close = abs(z0.real() - z1.real()) < tol && abs(z0.real() - z2.real()) < tol && abs(z1.real() - z2.real()) < tol;
       bool imag_close = abs(z0.imag() - z1.imag()) < tol && abs(z0.imag() - z2.imag()) < tol && abs(z1.imag() - z2.imag()) < tol;
       if (real_close && imag_close)
       {
           return z2;
       }

   } while(max_iterations--);

    // The idea is that if we can get abs(f) < eps, we should, but if we go through all these iterations
    // and abs(f) < sqrt(eps), then roundoff error simply does not allow that we can evaluate f to < eps
    // This is somewhat awkward as it isn't scale invariant, but using the Daubechies coefficient example code,
    // I found this condition generates correct roots, whereas the scale invariant condition discussed here:
    // https://scicomp.stackexchange.com/questions/30597/defining-a-condition-number-and-termination-criteria-for-newtons-method
    // allows nonroots to be passed off as roots.
    auto pair = g(z2);
    if (abs(pair.first) < sqrt(std::numeric_limits<Real>::epsilon()))
    {
        return z2;
    }

    return {std::numeric_limits<Real>::quiet_NaN(),
            std::numeric_limits<Real>::quiet_NaN()};
}
#endif


#if !defined(BOOST_NO_CXX17_IF_CONSTEXPR)
// https://stackoverflow.com/questions/48979861/numerically-stable-method-for-solving-quadratic-equations/50065711
namespace detail
{
    template<class T>
    inline T discriminant(T const & a, T const & b, T const & c)
    {
        T w = 4*a*c;
        T e = std::fma(-c, 4*a, w);
        T f = std::fma(b, b, -w);
        return f + e;
    }
}

template<class T>
auto quadratic_roots(T const& a, T const& b, T const& c)
{
    using std::copysign;
    using std::sqrt;
    if constexpr (std::is_integral<T>::value)
    {
        // What I want is to write:
        // return quadratic_roots(double(a), double(b), double(c));
        // but that doesn't compile.
        double nan = std::numeric_limits<double>::quiet_NaN();
        if(a==0)
        {
            if (b==0 && c != 0)
            {
                return std::pair<double, double>(nan, nan);
            }
            else if (b==0 && c==0)
            {
                return std::pair<double, double>(0,0);
            }
            return std::pair<double, double>(-c/b, -c/b);
        }
        if (b==0)
        {
            double x0_sq = -double(c)/double(a);
            if (x0_sq < 0) {
                return std::pair<double, double>(nan, nan);
            }
            double x0 = sqrt(x0_sq);
            return std::pair<double, double>(-x0,x0);
        }
        double discriminant = detail::discriminant(double(a), double(b), double(c));
        if (discriminant < 0)
        {
            return std::pair<double, double>(nan, nan);
        }
        double q = -(b + copysign(sqrt(discriminant), double(b)))/T(2);
        double x0 = q/a;
        double x1 = c/q;
        if (x0 < x1) {
            return std::pair<double, double>(x0, x1);
        }
        return std::pair<double, double>(x1, x0);
    }
    else if constexpr (std::is_floating_point<T>::value)
    {
        T nan = std::numeric_limits<T>::quiet_NaN();
        if(a==0)
        {
            if (b==0 && c != 0)
            {
                return std::pair<T, T>(nan, nan);
            }
            else if (b==0 && c==0)
            {
                return std::pair<T, T>(0,0);
            }
            return std::pair<T, T>(-c/b, -c/b);
        }
        if (b==0)
        {
            T x0_sq = -c/a;
            if (x0_sq < 0) {
                return std::pair<T, T>(nan, nan);
            }
            T x0 = sqrt(x0_sq);
            return std::pair<T, T>(-x0,x0);
        }
        T discriminant = detail::discriminant(a, b, c);
        // Is there a sane way to flush very small negative values to zero?
        // If there is I don't know of it.
        if (discriminant < 0)
        {
            return std::pair<T, T>(nan, nan);
        }
        T q = -(b + copysign(sqrt(discriminant), b))/T(2);
        T x0 = q/a;
        T x1 = c/q;
        if (x0 < x1)
        {
            return std::pair<T, T>(x0, x1);
        }
        return std::pair<T, T>(x1, x0);
    }
    else if constexpr (boost::is_complex<T>::value || boost::multiprecision::number_category<T>::value == boost::multiprecision::number_kind_complex)
    {
        typename T::value_type nan = std::numeric_limits<typename T::value_type>::quiet_NaN();
        if(a.real()==0 && a.imag() ==0)
        {
            using std::norm;
            if (b.real()==0 && b.imag() && norm(c) != 0)
            {
                return std::pair<T, T>({nan, nan}, {nan, nan});
            }
            else if (b.real()==0 && b.imag() && c.real() ==0 && c.imag() == 0)
            {
                return std::pair<T, T>({0,0},{0,0});
            }
            return std::pair<T, T>(-c/b, -c/b);
        }
        if (b.real()==0 && b.imag() == 0)
        {
            T x0_sq = -c/a;
            T x0 = sqrt(x0_sq);
            return std::pair<T, T>(-x0, x0);
        }
        // There's no fma for complex types:
        T discriminant = b*b - T(4)*a*c;
        T q = -(b + sqrt(discriminant))/T(2);
        return std::pair<T, T>(q/a, c/q);
    }
    else // Most likely the type is a boost.multiprecision.
    {    //There is no fma for multiprecision, and in addition it doesn't seem to be useful, so revert to the naive computation.
        T nan = std::numeric_limits<T>::quiet_NaN();
        if(a==0)
        {
            if (b==0 && c != 0)
            {
                return std::pair<T, T>(nan, nan);
            }
            else if (b==0 && c==0)
            {
                return std::pair<T, T>(0,0);
            }
            return std::pair<T, T>(-c/b, -c/b);
        }
        if (b==0)
        {
            T x0_sq = -c/a;
            if (x0_sq < 0) {
                return std::pair<T, T>(nan, nan);
            }
            T x0 = sqrt(x0_sq);
            return std::pair<T, T>(-x0,x0);
        }
        T discriminant = b*b - 4*a*c;
        if (discriminant < 0)
        {
            return std::pair<T, T>(nan, nan);
        }
        T q = -(b + copysign(sqrt(discriminant), b))/T(2);
        T x0 = q/a;
        T x1 = c/q;
        if (x0 < x1)
        {
            return std::pair<T, T>(x0, x1);
        }
        return std::pair<T, T>(x1, x0);
    }
}
#endif

} // namespace tools
} // namespace math
} // namespace boost

#endif // BOOST_MATH_TOOLS_NEWTON_SOLVER_HPP
