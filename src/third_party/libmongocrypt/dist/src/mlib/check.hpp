#ifndef MLIB_CHECK_HPP_INCLUDED
#define MLIB_CHECK_HPP_INCLUDED

#include <iostream>
#include <cstdio>
#include <string>

namespace mlib
{
namespace detail
{
struct check_info {
   const char *filename;
   int line;
   const char *expr;
};

struct nil {
};

template <typename Left> struct bound_lhs {
   check_info info;
   Left value;

#define DEFOP(Oper)                                                         \
   template <typename Rhs> nil operator Oper (Rhs rhs) const noexcept       \
   {                                                                        \
      if (value Oper rhs) {                                                 \
         return {};                                                         \
      }                                                                     \
      std::fprintf (stderr,                                                 \
                    "%s:%d: CHECK( %s ) failed!\n",                         \
                    info.filename,                                          \
                    info.line,                                              \
                    info.expr);                                             \
      std::cerr << "Expanded expression: " << value << " " #Oper " " << rhs \
                << '\n';                                                    \
      std::exit (2);                                                        \
   }
   DEFOP (==)
   DEFOP (!=)
   DEFOP (<)
   DEFOP (<=)
   DEFOP (>)
   DEFOP (>=)
#undef DEFOP
};

struct check_magic {
   check_info info;

   template <typename Oper>
   bound_lhs<Oper>
   operator->*(Oper op)
   {
      return bound_lhs<Oper>{info, op};
   }
};

struct check_consume {
   void
   operator= (nil)
   {
   }

   void
   operator= (bound_lhs<bool> const &l)
   {
      // Invoke the test for truthiness:
      (void) (l == true);
   }
};

/**
 * @brief Create an assertion that prints the expanded expression upon failure.
 *
 * Only supports simple comparison binary expressions, and plain boolean
 * expressions
 */
#define MLIB_CHECK(Cond)                                        \
   ::mlib::detail::check_consume{} =                            \
      ::mlib::detail::check_magic{                              \
         ::mlib::detail::check_info{__FILE__, __LINE__, #Cond}} \
         ->*Cond

} // namespace detail
} // namespace mlib

#endif // MLIB_CHECK_HPP_INCLUDED
