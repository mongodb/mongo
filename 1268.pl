#!/usr/bin/env perl

# Does the bulk of the work of fully-qualifying std:: symbols like
# std::string, std::map etc in headers. Use it like, first remove
# 'using namespace std' from bson/util/misc.h by hand, then run:
#   1268.pl bson/util/misc.h
# A misc.h file with fully-qualified std:: symbols will be written
# in place, leaving a .bak of the original.
#   This is crude and will replace some symbols that appear in
# comments but, nothing a little hand-checking won't clear up.
#   As a 2nd feature it lists std header files whose templates are
# used but aren't specifically #included, but where rather the code
# relies on other headers being #included first (eg if a header uses
# vector<> but doesn't #include <vector>, this will say so).


use strict;
use warnings;

# Call for in-place editing; make backups with a .bak suffix
$^I = '.bak';

# string
# vector
# list
# set
# map
# multimap
# stack
# queue
# deque
# pair
# make_pair
# auto_ptr

# type_info

# numeric_limits

# ios_base
# ios
# hex
# dec
# iostream
# ostream
# istream

# stringstream
# ostringstream
# istringstream
# strstream

# fstream
# ifstream
# ofstream

# cout
# cin
# cerr
# endl
# flush

# exception

# AMBI; boost or C++0x
# thread
# mutex
# tuple
# shared_ptr

# (use eg:) $element = $array_of_arrays[1][3];
my @substs = (
# [0]=pattern, [1]=replacement, [2]=associated header file
# ?<!std:: is, fail the match if preceded by std:: - try to be idempotent
['(?<!std::)\bmap<', "std::map<", "map"],
# hard to skip #include <string> !
['(?<!std::)\bstring\b', "std::string", "string"],
['(?<!std::)\bvector\s*<', "std::vector<", "vector"],
['(?<!std::)\blist\s*<', "std::list<", "list"],
['(?<!std::)\bset\s*<', "std::set<", "set"],
['(?<!std::)\bmap\s*<', "std::map<", "map"],
['(?<!std::)\bmultimap\s*<', "std::multimap<", "map"],
['(?<!std::)\bstack\s*<', "std::stack<", "stack"],
['(?<!std::)\bqueue\s*<', "std::queue<", "queue"],
['(?<!std::)\bdeque\s*<', "std::deque<", "deque"],
['(?<!std::)\bpair\s*<', "std::pair<", "utility"],
['(?<!std::)\bmake_pair\b', "std::make_pair", "utility"],
['(?<!std::)\bauto_ptr\s*<', "std::auto_ptr<", "memory"],

['(?<!std::)\btype_info\b', "std::type_info", ""],

['(?<!std::)\bnumeric_limits\s*<', "std::numeric_limits<", "limits"],

['(?<!std::)\bios_base\b', "std::ios_base", "iosfwd"],
['(?<!std::)\bios\b', "std::ios", "iosfwd"],
['(?<!std::)\bhex\b', "std::hex", "iosfwd"],
['(?<!std::)\bdec\b', "std::dec", "iosfwd"],
['(?<!std::)\biostream\b', "std::iostream", "iosfwd"],
['(?<!std::)\bostream\b', "std::ostream", "iosfwd"],
['(?<!std::)\bistream\b', "std::istream", "iosfwd"],

['(?<!std::)\bstringstream\b', "std::stringstream", "sstream"],
['(?<!std::)\bostringstream\b', "std::ostringstream", "sstream"],
['(?<!std::)\bistringstream\b', "std::istringstream", "sstream"],
['(?<!std::)\bstrstream\b', "std::strstream", "strstream"],

['(?<!std::)\bfstream\b', "std::fstream", "fstream"],
['(?<!std::)\bifstream\b', "std::ifstream", "fstream"],
['(?<!std::)\bofstream\b', "std::ofstream", "fstream"],

['(?<!std::)\bcout\b', "std::cout", "iosfwd"],
['(?<!std::)\bcin\b', "std::cin", "iosfwd"],
['(?<!std::)\bcerr\b', "std::cerr", "iosfwd"],
['(?<!std::)\bendl\b', "std::endl", "iosfwd"],
# a post lookaround; the next thing cannot be a '('
['(?<!std::)\bflush\b(?!\s*\()', "std::flush", "iosfwd"],
['(?<!std::)\bexception\b', "std::exception", "exception"]
    );

my %g_have_header = ();
my %g_need_header = ();

while (<>) {
    if (/\s*\#\s*include\s*/) {
        foreach my $arr (@substs) {
            $g_have_header{$arr->[2]} = 1;
            # ... but don't do replace on lines with '#include'
        }
    }
    else {
        foreach my $arr (@substs) {
            s/$arr->[0]/$arr->[1]/g;

            # whether we made it that way or it was already
            if (/$arr->[1]/) {
                $g_need_header{$arr->[2]} = 1;
            }
        }
    }
    print;
}

# set difference
for my $key (keys %g_have_header) {
    delete $g_need_header{$key};
}

if (keys %g_need_header) {
    print "Uses but does not explicitly include:\n\n";

    for my $key (keys %g_need_header) {
        print "#include <$key>\n";
    }
}
