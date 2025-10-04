/*=============================================================================
    Copyright (c) 2001-2013 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#include <boost/spirit/home/x3/support/context.hpp>
#include <iostream>

using boost::spirit::x3::make_context;
using boost::spirit::x3::get;

int bb;
int cc;

struct b_ctx;
struct c_ctx;

template <typename Context>
void a(Context const& context)
{
    bb = get<b_ctx>(context);
    cc = get<c_ctx>(context);
}

template <typename Context>
void b(Context const& context)
{
    int bi = 123;
    a(make_context<b_ctx>(bi, context));
}

void c()
{
    int ci = 456;
    b(make_context<c_ctx>(ci));
}

void test()
{
    c();

//  MSVC generates this code:
//      mov	DWORD PTR ?bb@@3HA, 123
//      mov	DWORD PTR ?cc@@3HA, 456
//
//  GCC generates this code:
//      movl    $123,   _bb
//      movl    $456,   _cc
}

