#!/usr/bin/perl -w

use strict;
use warnings;

my $header = <<'END';
/*
 * Copyright 2016-2018 Andrey Semashev
 *
 * Distributed under the Boost Software License, Version 1.0.
 * See http://www.boost.org/LICENSE_1_0.txt
 */

#ifndef BOOST_WINAPI_ERROR_CODES_HPP_INCLUDED_
#define BOOST_WINAPI_ERROR_CODES_HPP_INCLUDED_

#include <boost/winapi/basic_types.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace winapi {

END

my $footer = <<'END';

} // namespace winapi
} // namespace boost

#endif // BOOST_WINAPI_ERROR_CODES_HPP_INCLUDED_
END

print $header;

while (<>)
{
    my $line = $_;
    chomp($line);
    if ($line =~ /^\s*#\s*define\s+([a-zA-Z_\d]+)\s+(0[xX][[:xdigit:]]+|\d+|[a-zA-Z_\d]+)[lLuU]*\s*(\/\/.*|\/\*.*)?$/)
    {
        # We define some of the constants in other headers
        if ($1 ne "FORCEINLINE" && $1 ne "WAIT_TIMEOUT")
        {
            my $value = $2;
            print "BOOST_CONSTEXPR_OR_CONST DWORD_ ", $1 , "_ = ";
            if ($value =~ /0[xX][[:xdigit:]]+|\d+/)
            {
                print $value;
            }
            else
            {
                print $value, "_";
            }
            print ";\n";
        }
    }
}

print $footer;
