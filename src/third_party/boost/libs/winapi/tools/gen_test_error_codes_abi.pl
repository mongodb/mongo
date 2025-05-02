#!/usr/bin/perl -w

use strict;
use warnings;

my $header = <<'END';
/*
 *             Copyright Andrey Semashev 2018.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   error_codes_abi.cpp
 * \author Andrey Semashev
 * \date   09.03.2018
 *
 * \brief  This file contains ABI test for error_codes.hpp
 */

#include <boost/winapi/error_codes.hpp>
#include <windows.h>
#include <winerror.h>
#include <boost/predef/platform/windows_uwp.h>
#include "abi_test_tools.hpp"

int main()
{
END

my $footer = <<'END';

    return boost::report_errors();
}
END

print $header;

while (<>)
{
    my $line = $_;
    chomp($line);
    if ($line =~ /^\s*BOOST_CONSTEXPR_OR_CONST\s+DWORD_\s+([a-zA-Z_\d]+)_\s+.*$/)
    {
        print "#if ";
        # Some constants have different values in different Windows SDKs
        if ($1 eq "ERROR_IPSEC_IKE_NEG_STATUS_END")
        {
            print "BOOST_PLAT_WINDOWS_SDK_VERSION >= BOOST_WINAPI_WINDOWS_SDK_6_0 && ";
        }
        print "defined(", $1 , ")\n";
        print "    BOOST_WINAPI_TEST_CONSTANT(", $1 , ");\n";
        print "#endif\n";
    }
}

print $footer;
