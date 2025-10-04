//  Copyright John Maddock 2019.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef __GNUC__
#error "Compiler is not GCC"
#endif
#if __GNUC__ < 9
#error "Older GCC versions don't support -fconstexpr-ops-limit"
#endif

int main()
{
   return 0;
}
