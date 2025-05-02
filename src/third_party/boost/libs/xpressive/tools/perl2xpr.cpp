///////////////////////////////////////////////////////////////////////////////
// perl2xpr.cpp
//      A utility for translating a Perl regular expression into an
//      xpressive static regular expression.
//
//  Copyright 2007 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <stack>
#include <string>
#include <iostream>
#include <boost/xpressive/xpressive_static.hpp>
#include <boost/xpressive/regex_actions.hpp>

namespace x = boost::xpressive;
using namespace x;

int main(int argc, char *argv[])
{
    int i = 1, j = 1;
    bool nocase = false;
    char const *dot = " ~_n ";
    char const *bos = " bos ";
    char const *eos = " eos ";

    for(; i < argc && '-' == *argv[i]; argv[i][++j]? 0: (j=1,++i))
    {
        switch(argv[i][j])
        {
        case 'i':           // perl /i modifier
            nocase = true;
            break;
        case 's':           // perl /s modifier
            dot = " _ ";
            break;
        case 'm':           // perl /m modifier
            bos = " bol ";
            eos = " eol ";
            break;
        default:
            std::cerr << "Unknown option : " << argv[i] << std::endl;
            return -1;
        }
    }

    if(i == argc)
    {
        std::cerr << "Usage:\n    perl2xpr [-i] [-s] [-m] 're'\n";
        return -1;
    }

    // Local variables used by the semantic actions below
    local<int> mark_nbr;
    local<std::string> tmp;
    local<std::stack<std::string> > strings;

    // The rules in the dynamic regex grammar
    cregex regex, alts, seq, quant, repeat, atom, escape, group, lit, charset, setelem;

    lit     = ~(set='.','^','$','*','+','?','(',')','{','}','[',']','\\','|')
            ;

    escape  = as_xpr('b')               [top(strings) += " _b "]
            | as_xpr('B')               [top(strings) += " ~_b "]
            | as_xpr('d')               [top(strings) += " _d "]
            | as_xpr('D')               [top(strings) += " ~_d "]
            | as_xpr('s')               [top(strings) += " _s "]
            | as_xpr('S')               [top(strings) += " ~_s "]
            | as_xpr('w')               [top(strings) += " _w "]
            | as_xpr('W')               [top(strings) += " ~_w "]
            | _d                        [top(strings) += " s" + _ + " "]
            | _                         [top(strings) += " as_xpr('" + _ + "') "]
            ;

    group   = (
                  as_xpr("?:")          [top(strings) += " ( "]
                | as_xpr("?i:")         [top(strings) += " icase( "]
                | as_xpr("?>")          [top(strings) += " keep( "]
                | as_xpr("?=")          [top(strings) += " before( "]
                | as_xpr("?!")          [top(strings) += " ~before( "]
                | as_xpr("?<=")         [top(strings) += " after( "]
                | as_xpr("?<!")         [top(strings) += " ~after( "]
                | nil                   [top(strings) += " ( s" + as<std::string>(++mark_nbr) + "= "]
              )
            >> x::ref(regex)
            >> as_xpr(')')              [top(strings) += " ) "]
            ;

    setelem = as_xpr('\\') >> _         [top(strings) += " as_xpr('" + _ + "') "]
            | "[:" >> !as_xpr('^')      [top(strings) += "~"]
                >> (+_w)                [top(strings) += _ ]
                >> ":]"
            | (
                   (s1=~as_xpr(']')) 
                >> '-'
                >> (s2=~as_xpr(']'))
              )                         [top(strings) += "range('" + s1 + "','" + s2 + "')"]
            ;

    charset = !as_xpr('^')              [top(strings) += " ~ "]
            >> nil                      [top(strings) += " set[ "]
            >> (
                    setelem
                  | (~as_xpr(']'))      [top(strings) += " as_xpr('" + _ + "') "]
               )
            >>*(
                    nil                 [top(strings) += " | "]
                 >> (
                        setelem
                      | (~as_xpr(']'))  [top(strings) += "'" + _ + "'"]
                    )
               )
            >> as_xpr(']')              [top(strings) += " ] "]
            ;

    atom    = (
                  +(lit >> ~before((set='*','+','?','{')))
                | lit
              )                         [top(strings) += " as_xpr(\"" + _ + "\") "]
            | as_xpr('.')               [top(strings) += dot]
            | as_xpr('^')               [top(strings) += bos]
            | as_xpr('$')               [top(strings) += eos]
            | '\\' >> escape
            | '(' >> group
            | '[' >> charset
            ;

    repeat  = as_xpr('{')               [tmp = " repeat<"]
            >> (+_d)                    [tmp += _]
            >> !(
                    as_xpr(',')         [tmp += ","]
                 >> (
                        (+_d)           [tmp += _]
                      | nil             [tmp += "inf"]
                    )
                )
            >> as_xpr('}')              [top(strings) = tmp + ">( " + top(strings) + " ) "]
            ;

    quant   = nil                       [push(strings, "")]
            >> atom
            >> !(
                    (
                        as_xpr("*")     [insert(top(strings), 0, " * ")] // [strings->*top()->*insert(0, " * ")]
                      | as_xpr("+")     [insert(top(strings), 0, " + ")] // [strings->*top()->*insert(0, " + ")]
                      | as_xpr("?")     [insert(top(strings), 0, " ! ")] // [strings->*top()->*insert(0, " ! ")]
                      | repeat
                    )
                 >> !as_xpr('?')        [insert(top(strings), 0, " - ")]
                )
            >> nil                      [tmp = top(strings), pop(strings), top(strings) += tmp]
            ;

    seq     = quant
            >> *(
                    nil                 [top(strings) += " >> "]
                 >> quant
                )
            ;

    alts    = seq
            >> *(
                    as_xpr('|')         [top(strings) += " | "]
                 >> seq
                )
            ;

    regex   = alts
            ;

    strings.get().push("");
    if(!regex_match(argv[i], regex))
    {
        std::cerr << "ERROR: unrecognized regular expression" << std::endl;
        return -1;
    }
    else if(nocase)
    {
        std::cout << "icase( " << strings.get().top() << " )" << std::endl;
    }
    else
    {
        std::cout << strings.get().top() << std::endl;
    }

    return 0;
}
