
#line 1 "util/ipv6_parser.rl"
#ifndef __IPv6_Parser_RL__
#define __IPv6_Parser_RL__

/*
 *    Copyright 2009-2011 Intrusion Inc
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <deque>

#include "ragel_base.h"


// Forward Decl



// Here are some web sites that describe the various forms of valid IPv6 Addresses
// http://docsrv.sco.com/SDK_netapi/sockC.IPv6addressnotation.html
// http://www.ecst.csuchico.edu/~dranch/NETWORKS/IPV6/sld013.htm
// http://www.tcpipguide.com/free/t_IPv6AddressandAddressNotationandPrefixRepresentati-3.htm
// http://www.tcpipguide.com/free/t_IPv6AddressandAddressNotationandPrefixRepresentati-4.htm



#line 180 "util/ipv6_parser.rl"



// Ragel Table Data

#line 49 "util/ipv6_parser.h"
static const int ipv6_start = 1;
static const int ipv6_first_final = 121;
static const int ipv6_error = 0;

static const int ipv6_en_main = 1;


#line 185 "util/ipv6_parser.rl"



class IPv6_Parser : public RagelBase<uint8_t>
{
 private:

  public:

    IPv6_Parser() : RagelBase<uint8_t>()
    {
    }

    virtual ~IPv6_Parser()
    {
    }

    /// Parse the given string into a low and high integer range or list.
    /// \param p_string A pointer to the text to convert.
    /// \returns true on a successful parse, false on syntax error.
    ///
    bool parse(std::string& p_string)
    {
        return parse(p_string.c_str());
    }

    bool parse(const char* p_string)
    {
        init();
    
        // Calculate the length (include null terminator)
        size_t len = strlen(p_string) + 1;
    
        // Setup the buffer and parse
        setBuffer((const uint8_t*)p_string, len, true);
    
        return execute();
    }


    IP_Addr getIPv6(void) { return m_working_addr; }


  private:

    virtual void init(void)
    {
        // Call the parent init
        RagelBase<uint8_t>::init();

        start_new_ip();

        m_error = false;

        
#line 113 "util/ipv6_parser.h"
	{
	cs = ipv6_start;
	}

#line 240 "util/ipv6_parser.rl"
    }

    void ragel_exec(void)
    {
        // Ragel write exec begin
        
#line 125 "util/ipv6_parser.h"
	{
	if ( p == pe )
		goto _test_eof;
	switch ( cs )
	{
case 1:
	if ( (*p) == 58u )
		goto st111;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr1;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr1;
	} else
		goto tr1;
	goto tr0;
tr0:
#line 164 "util/ipv6_parser.rl"
	{ parse_error(); }
	goto st0;
#line 147 "util/ipv6_parser.h"
st0:
cs = 0;
	goto _out;
tr1:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st2;
st2:
	if ( ++p == pe )
		goto _test_eof2;
case 2:
#line 159 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr4;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st3;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st3;
	} else
		goto st3;
	goto tr0;
st3:
	if ( ++p == pe )
		goto _test_eof3;
case 3:
	if ( (*p) == 58u )
		goto tr4;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st4;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st4;
	} else
		goto st4;
	goto tr0;
st4:
	if ( ++p == pe )
		goto _test_eof4;
case 4:
	if ( (*p) == 58u )
		goto tr4;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st5;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st5;
	} else
		goto st5;
	goto tr0;
st5:
	if ( ++p == pe )
		goto _test_eof5;
case 5:
	if ( (*p) == 58u )
		goto tr4;
	goto tr0;
tr4:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st6;
st6:
	if ( ++p == pe )
		goto _test_eof6;
case 6:
#line 216 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st102;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr7;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr7;
	} else
		goto tr7;
	goto tr0;
tr7:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st7;
st7:
	if ( ++p == pe )
		goto _test_eof7;
case 7:
#line 236 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr10;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st8;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st8;
	} else
		goto st8;
	goto tr0;
st8:
	if ( ++p == pe )
		goto _test_eof8;
case 8:
	if ( (*p) == 58u )
		goto tr10;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st9;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st9;
	} else
		goto st9;
	goto tr0;
st9:
	if ( ++p == pe )
		goto _test_eof9;
case 9:
	if ( (*p) == 58u )
		goto tr10;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st10;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st10;
	} else
		goto st10;
	goto tr0;
st10:
	if ( ++p == pe )
		goto _test_eof10;
case 10:
	if ( (*p) == 58u )
		goto tr10;
	goto tr0;
tr10:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st11;
st11:
	if ( ++p == pe )
		goto _test_eof11;
case 11:
#line 293 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st93;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr13;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr13;
	} else
		goto tr13;
	goto tr0;
tr13:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st12;
st12:
	if ( ++p == pe )
		goto _test_eof12;
case 12:
#line 313 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr16;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st13;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st13;
	} else
		goto st13;
	goto tr0;
st13:
	if ( ++p == pe )
		goto _test_eof13;
case 13:
	if ( (*p) == 58u )
		goto tr16;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st14;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st14;
	} else
		goto st14;
	goto tr0;
st14:
	if ( ++p == pe )
		goto _test_eof14;
case 14:
	if ( (*p) == 58u )
		goto tr16;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st15;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st15;
	} else
		goto st15;
	goto tr0;
st15:
	if ( ++p == pe )
		goto _test_eof15;
case 15:
	if ( (*p) == 58u )
		goto tr16;
	goto tr0;
tr16:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st16;
st16:
	if ( ++p == pe )
		goto _test_eof16;
case 16:
#line 370 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st84;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr19;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr19;
	} else
		goto tr19;
	goto tr0;
tr19:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st17;
st17:
	if ( ++p == pe )
		goto _test_eof17;
case 17:
#line 390 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr22;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st18;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st18;
	} else
		goto st18;
	goto tr0;
st18:
	if ( ++p == pe )
		goto _test_eof18;
case 18:
	if ( (*p) == 58u )
		goto tr22;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st19;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st19;
	} else
		goto st19;
	goto tr0;
st19:
	if ( ++p == pe )
		goto _test_eof19;
case 19:
	if ( (*p) == 58u )
		goto tr22;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st20;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st20;
	} else
		goto st20;
	goto tr0;
st20:
	if ( ++p == pe )
		goto _test_eof20;
case 20:
	if ( (*p) == 58u )
		goto tr22;
	goto tr0;
tr22:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st21;
st21:
	if ( ++p == pe )
		goto _test_eof21;
case 21:
#line 447 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st75;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr25;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr25;
	} else
		goto tr25;
	goto tr0;
tr25:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st22;
st22:
	if ( ++p == pe )
		goto _test_eof22;
case 22:
#line 467 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr28;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st23;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st23;
	} else
		goto st23;
	goto tr0;
st23:
	if ( ++p == pe )
		goto _test_eof23;
case 23:
	if ( (*p) == 58u )
		goto tr28;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st24;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st24;
	} else
		goto st24;
	goto tr0;
st24:
	if ( ++p == pe )
		goto _test_eof24;
case 24:
	if ( (*p) == 58u )
		goto tr28;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st25;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st25;
	} else
		goto st25;
	goto tr0;
st25:
	if ( ++p == pe )
		goto _test_eof25;
case 25:
	if ( (*p) == 58u )
		goto tr28;
	goto tr0;
tr28:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st26;
st26:
	if ( ++p == pe )
		goto _test_eof26;
case 26:
#line 524 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st66;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr31;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr31;
	} else
		goto tr31;
	goto tr0;
tr31:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st27;
st27:
	if ( ++p == pe )
		goto _test_eof27;
case 27:
#line 544 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr34;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st28;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st28;
	} else
		goto st28;
	goto tr0;
st28:
	if ( ++p == pe )
		goto _test_eof28;
case 28:
	if ( (*p) == 58u )
		goto tr34;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st29;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st29;
	} else
		goto st29;
	goto tr0;
st29:
	if ( ++p == pe )
		goto _test_eof29;
case 29:
	if ( (*p) == 58u )
		goto tr34;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st30;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st30;
	} else
		goto st30;
	goto tr0;
st30:
	if ( ++p == pe )
		goto _test_eof30;
case 30:
	if ( (*p) == 58u )
		goto tr34;
	goto tr0;
tr34:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st31;
st31:
	if ( ++p == pe )
		goto _test_eof31;
case 31:
#line 601 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st60;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr37;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr39;
	} else
		goto tr39;
	goto tr0;
tr37:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st32;
st32:
	if ( ++p == pe )
		goto _test_eof32;
case 32:
#line 623 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 46u: goto st33;
		case 58u: goto tr42;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st49;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st59;
	} else
		goto st59;
	goto tr0;
st33:
	if ( ++p == pe )
		goto _test_eof33;
case 33:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st34;
	goto tr0;
st34:
	if ( ++p == pe )
		goto _test_eof34;
case 34:
	if ( (*p) == 46u )
		goto st35;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st47;
	goto tr0;
st35:
	if ( ++p == pe )
		goto _test_eof35;
case 35:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st36;
	goto tr0;
st36:
	if ( ++p == pe )
		goto _test_eof36;
case 36:
	if ( (*p) == 46u )
		goto st37;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st45;
	goto tr0;
st37:
	if ( ++p == pe )
		goto _test_eof37;
case 37:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st38;
	goto tr0;
st38:
	if ( ++p == pe )
		goto _test_eof38;
case 38:
	switch( (*p) ) {
		case 0u: goto tr51;
		case 47u: goto tr52;
	}
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st43;
	goto tr0;
tr66:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st121;
tr51:
#line 45 "util/ipv6_parser.rl"
	{ ipv4_end(); }
	goto st121;
tr55:
#line 53 "util/ipv6_parser.rl"
	{ prefix_end(); }
	goto st121;
tr74:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st121;
st121:
	if ( ++p == pe )
		goto _test_eof121;
case 121:
#line 707 "util/ipv6_parser.h"
	goto tr0;
tr67:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st39;
tr52:
#line 45 "util/ipv6_parser.rl"
	{ ipv4_end(); }
	goto st39;
tr75:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st39;
st39:
	if ( ++p == pe )
		goto _test_eof39;
case 39:
#line 725 "util/ipv6_parser.h"
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr54;
	goto tr0;
tr54:
#line 53 "util/ipv6_parser.rl"
	{ prefix_start(); }
	goto st40;
st40:
	if ( ++p == pe )
		goto _test_eof40;
case 40:
#line 737 "util/ipv6_parser.h"
	if ( (*p) == 0u )
		goto tr55;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st41;
	goto tr0;
st41:
	if ( ++p == pe )
		goto _test_eof41;
case 41:
	if ( (*p) == 0u )
		goto tr55;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st42;
	goto tr0;
st42:
	if ( ++p == pe )
		goto _test_eof42;
case 42:
	if ( (*p) == 0u )
		goto tr55;
	goto tr0;
st43:
	if ( ++p == pe )
		goto _test_eof43;
case 43:
	switch( (*p) ) {
		case 0u: goto tr51;
		case 47u: goto tr52;
	}
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st44;
	goto tr0;
st44:
	if ( ++p == pe )
		goto _test_eof44;
case 44:
	switch( (*p) ) {
		case 0u: goto tr51;
		case 47u: goto tr52;
	}
	goto tr0;
st45:
	if ( ++p == pe )
		goto _test_eof45;
case 45:
	if ( (*p) == 46u )
		goto st37;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st46;
	goto tr0;
st46:
	if ( ++p == pe )
		goto _test_eof46;
case 46:
	if ( (*p) == 46u )
		goto st37;
	goto tr0;
st47:
	if ( ++p == pe )
		goto _test_eof47;
case 47:
	if ( (*p) == 46u )
		goto st35;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto st48;
	goto tr0;
st48:
	if ( ++p == pe )
		goto _test_eof48;
case 48:
	if ( (*p) == 46u )
		goto st35;
	goto tr0;
st49:
	if ( ++p == pe )
		goto _test_eof49;
case 49:
	switch( (*p) ) {
		case 46u: goto st33;
		case 58u: goto tr42;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st50;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st58;
	} else
		goto st58;
	goto tr0;
st50:
	if ( ++p == pe )
		goto _test_eof50;
case 50:
	switch( (*p) ) {
		case 46u: goto st33;
		case 58u: goto tr42;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st51;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st51;
	} else
		goto st51;
	goto tr0;
st51:
	if ( ++p == pe )
		goto _test_eof51;
case 51:
	if ( (*p) == 58u )
		goto tr42;
	goto tr0;
tr42:
#line 48 "util/ipv6_parser.rl"
	{ h_front_push(); }
	goto st52;
st52:
	if ( ++p == pe )
		goto _test_eof52;
case 52:
#line 860 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto st57;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr64;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr64;
	} else
		goto tr64;
	goto tr0;
tr64:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st53;
st53:
	if ( ++p == pe )
		goto _test_eof53;
case 53:
#line 880 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr66;
		case 47u: goto tr67;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st54;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st54;
	} else
		goto st54;
	goto tr0;
st54:
	if ( ++p == pe )
		goto _test_eof54;
case 54:
	switch( (*p) ) {
		case 0u: goto tr66;
		case 47u: goto tr67;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st55;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st55;
	} else
		goto st55;
	goto tr0;
st55:
	if ( ++p == pe )
		goto _test_eof55;
case 55:
	switch( (*p) ) {
		case 0u: goto tr66;
		case 47u: goto tr67;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st56;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st56;
	} else
		goto st56;
	goto tr0;
st56:
	if ( ++p == pe )
		goto _test_eof56;
case 56:
	switch( (*p) ) {
		case 0u: goto tr66;
		case 47u: goto tr67;
	}
	goto tr0;
st57:
	if ( ++p == pe )
		goto _test_eof57;
case 57:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	goto tr0;
st58:
	if ( ++p == pe )
		goto _test_eof58;
case 58:
	if ( (*p) == 58u )
		goto tr42;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st51;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st51;
	} else
		goto st51;
	goto tr0;
st59:
	if ( ++p == pe )
		goto _test_eof59;
case 59:
	if ( (*p) == 58u )
		goto tr42;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st58;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st58;
	} else
		goto st58;
	goto tr0;
st60:
	if ( ++p == pe )
		goto _test_eof60;
case 60:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr73;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr73;
	} else
		goto tr73;
	goto tr0;
tr73:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st61;
st61:
	if ( ++p == pe )
		goto _test_eof61;
case 61:
#line 1001 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st62;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st62;
	} else
		goto st62;
	goto tr0;
st62:
	if ( ++p == pe )
		goto _test_eof62;
case 62:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st63;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st63;
	} else
		goto st63;
	goto tr0;
st63:
	if ( ++p == pe )
		goto _test_eof63;
case 63:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st64;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st64;
	} else
		goto st64;
	goto tr0;
st64:
	if ( ++p == pe )
		goto _test_eof64;
case 64:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
	}
	goto tr0;
tr39:
#line 48 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st65;
st65:
	if ( ++p == pe )
		goto _test_eof65;
case 65:
#line 1066 "util/ipv6_parser.h"
	if ( (*p) == 58u )
		goto tr42;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st59;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st59;
	} else
		goto st59;
	goto tr0;
st66:
	if ( ++p == pe )
		goto _test_eof66;
case 66:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr79;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr80;
	} else
		goto tr80;
	goto tr0;
tr79:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st67;
st67:
	if ( ++p == pe )
		goto _test_eof67;
case 67:
#line 1105 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st68;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st73;
	} else
		goto st73;
	goto tr0;
st68:
	if ( ++p == pe )
		goto _test_eof68;
case 68:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st69;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st72;
	} else
		goto st72;
	goto tr0;
st69:
	if ( ++p == pe )
		goto _test_eof69;
case 69:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st70;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st70;
	} else
		goto st70;
	goto tr0;
st70:
	if ( ++p == pe )
		goto _test_eof70;
case 70:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	goto tr0;
tr82:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st71;
st71:
	if ( ++p == pe )
		goto _test_eof71;
case 71:
#line 1177 "util/ipv6_parser.h"
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr73;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr73;
	} else
		goto tr73;
	goto tr0;
st72:
	if ( ++p == pe )
		goto _test_eof72;
case 72:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st70;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st70;
	} else
		goto st70;
	goto tr0;
st73:
	if ( ++p == pe )
		goto _test_eof73;
case 73:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st72;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st72;
	} else
		goto st72;
	goto tr0;
tr80:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st74;
st74:
	if ( ++p == pe )
		goto _test_eof74;
case 74:
#line 1231 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st73;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st73;
	} else
		goto st73;
	goto tr0;
st75:
	if ( ++p == pe )
		goto _test_eof75;
case 75:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr87;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr88;
	} else
		goto tr88;
	goto tr0;
tr87:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st76;
st76:
	if ( ++p == pe )
		goto _test_eof76;
case 76:
#line 1273 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st77;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st82;
	} else
		goto st82;
	goto tr0;
st77:
	if ( ++p == pe )
		goto _test_eof77;
case 77:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st78;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st81;
	} else
		goto st81;
	goto tr0;
st78:
	if ( ++p == pe )
		goto _test_eof78;
case 78:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st79;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st79;
	} else
		goto st79;
	goto tr0;
st79:
	if ( ++p == pe )
		goto _test_eof79;
case 79:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	goto tr0;
tr90:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st80;
st80:
	if ( ++p == pe )
		goto _test_eof80;
case 80:
#line 1345 "util/ipv6_parser.h"
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr79;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr80;
	} else
		goto tr80;
	goto tr0;
st81:
	if ( ++p == pe )
		goto _test_eof81;
case 81:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st79;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st79;
	} else
		goto st79;
	goto tr0;
st82:
	if ( ++p == pe )
		goto _test_eof82;
case 82:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st81;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st81;
	} else
		goto st81;
	goto tr0;
tr88:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st83;
st83:
	if ( ++p == pe )
		goto _test_eof83;
case 83:
#line 1399 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr90;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st82;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st82;
	} else
		goto st82;
	goto tr0;
st84:
	if ( ++p == pe )
		goto _test_eof84;
case 84:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr95;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr96;
	} else
		goto tr96;
	goto tr0;
tr95:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st85;
st85:
	if ( ++p == pe )
		goto _test_eof85;
case 85:
#line 1441 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st86;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st91;
	} else
		goto st91;
	goto tr0;
st86:
	if ( ++p == pe )
		goto _test_eof86;
case 86:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st87;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st90;
	} else
		goto st90;
	goto tr0;
st87:
	if ( ++p == pe )
		goto _test_eof87;
case 87:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st88;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st88;
	} else
		goto st88;
	goto tr0;
st88:
	if ( ++p == pe )
		goto _test_eof88;
case 88:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	goto tr0;
tr98:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st89;
st89:
	if ( ++p == pe )
		goto _test_eof89;
case 89:
#line 1513 "util/ipv6_parser.h"
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr87;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr88;
	} else
		goto tr88;
	goto tr0;
st90:
	if ( ++p == pe )
		goto _test_eof90;
case 90:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st88;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st88;
	} else
		goto st88;
	goto tr0;
st91:
	if ( ++p == pe )
		goto _test_eof91;
case 91:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st90;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st90;
	} else
		goto st90;
	goto tr0;
tr96:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st92;
st92:
	if ( ++p == pe )
		goto _test_eof92;
case 92:
#line 1567 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr98;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st91;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st91;
	} else
		goto st91;
	goto tr0;
st93:
	if ( ++p == pe )
		goto _test_eof93;
case 93:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr103;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr104;
	} else
		goto tr104;
	goto tr0;
tr103:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st94;
st94:
	if ( ++p == pe )
		goto _test_eof94;
case 94:
#line 1609 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st95;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st100;
	} else
		goto st100;
	goto tr0;
st95:
	if ( ++p == pe )
		goto _test_eof95;
case 95:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st96;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st99;
	} else
		goto st99;
	goto tr0;
st96:
	if ( ++p == pe )
		goto _test_eof96;
case 96:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st97;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st97;
	} else
		goto st97;
	goto tr0;
st97:
	if ( ++p == pe )
		goto _test_eof97;
case 97:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	goto tr0;
tr106:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st98;
st98:
	if ( ++p == pe )
		goto _test_eof98;
case 98:
#line 1681 "util/ipv6_parser.h"
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr95;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr96;
	} else
		goto tr96;
	goto tr0;
st99:
	if ( ++p == pe )
		goto _test_eof99;
case 99:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st97;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st97;
	} else
		goto st97;
	goto tr0;
st100:
	if ( ++p == pe )
		goto _test_eof100;
case 100:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st99;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st99;
	} else
		goto st99;
	goto tr0;
tr104:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st101;
st101:
	if ( ++p == pe )
		goto _test_eof101;
case 101:
#line 1735 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr106;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st100;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st100;
	} else
		goto st100;
	goto tr0;
st102:
	if ( ++p == pe )
		goto _test_eof102;
case 102:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr111;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr112;
	} else
		goto tr112;
	goto tr0;
tr111:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st103;
st103:
	if ( ++p == pe )
		goto _test_eof103;
case 103:
#line 1777 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st104;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st109;
	} else
		goto st109;
	goto tr0;
st104:
	if ( ++p == pe )
		goto _test_eof104;
case 104:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st105;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st108;
	} else
		goto st108;
	goto tr0;
st105:
	if ( ++p == pe )
		goto _test_eof105;
case 105:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st106;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st106;
	} else
		goto st106;
	goto tr0;
st106:
	if ( ++p == pe )
		goto _test_eof106;
case 106:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	goto tr0;
tr114:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st107;
st107:
	if ( ++p == pe )
		goto _test_eof107;
case 107:
#line 1849 "util/ipv6_parser.h"
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr103;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr104;
	} else
		goto tr104;
	goto tr0;
st108:
	if ( ++p == pe )
		goto _test_eof108;
case 108:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st106;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st106;
	} else
		goto st106;
	goto tr0;
st109:
	if ( ++p == pe )
		goto _test_eof109;
case 109:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st108;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st108;
	} else
		goto st108;
	goto tr0;
tr112:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st110;
st110:
	if ( ++p == pe )
		goto _test_eof110;
case 110:
#line 1903 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr114;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st109;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st109;
	} else
		goto st109;
	goto tr0;
st111:
	if ( ++p == pe )
		goto _test_eof111;
case 111:
	if ( (*p) == 58u )
		goto st112;
	goto tr0;
st112:
	if ( ++p == pe )
		goto _test_eof112;
case 112:
	switch( (*p) ) {
		case 0u: goto st121;
		case 47u: goto st39;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr120;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr121;
	} else
		goto tr121;
	goto tr0;
tr120:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
#line 45 "util/ipv6_parser.rl"
	{ ipv4_start(); }
	goto st113;
st113:
	if ( ++p == pe )
		goto _test_eof113;
case 113:
#line 1952 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st114;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st119;
	} else
		goto st119;
	goto tr0;
st114:
	if ( ++p == pe )
		goto _test_eof114;
case 114:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st115;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st118;
	} else
		goto st118;
	goto tr0;
st115:
	if ( ++p == pe )
		goto _test_eof115;
case 115:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 46u: goto st33;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st116;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st116;
	} else
		goto st116;
	goto tr0;
st116:
	if ( ++p == pe )
		goto _test_eof116;
case 116:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	goto tr0;
tr123:
#line 51 "util/ipv6_parser.rl"
	{ h_back_push(); }
	goto st117;
st117:
	if ( ++p == pe )
		goto _test_eof117;
case 117:
#line 2024 "util/ipv6_parser.h"
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr111;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr112;
	} else
		goto tr112;
	goto tr0;
st118:
	if ( ++p == pe )
		goto _test_eof118;
case 118:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st116;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st116;
	} else
		goto st116;
	goto tr0;
st119:
	if ( ++p == pe )
		goto _test_eof119;
case 119:
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st118;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st118;
	} else
		goto st118;
	goto tr0;
tr121:
#line 51 "util/ipv6_parser.rl"
	{ h_start(); }
	goto st120;
st120:
	if ( ++p == pe )
		goto _test_eof120;
case 120:
#line 2078 "util/ipv6_parser.h"
	switch( (*p) ) {
		case 0u: goto tr74;
		case 47u: goto tr75;
		case 58u: goto tr123;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto st119;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto st119;
	} else
		goto st119;
	goto tr0;
	}
	_test_eof2: cs = 2; goto _test_eof; 
	_test_eof3: cs = 3; goto _test_eof; 
	_test_eof4: cs = 4; goto _test_eof; 
	_test_eof5: cs = 5; goto _test_eof; 
	_test_eof6: cs = 6; goto _test_eof; 
	_test_eof7: cs = 7; goto _test_eof; 
	_test_eof8: cs = 8; goto _test_eof; 
	_test_eof9: cs = 9; goto _test_eof; 
	_test_eof10: cs = 10; goto _test_eof; 
	_test_eof11: cs = 11; goto _test_eof; 
	_test_eof12: cs = 12; goto _test_eof; 
	_test_eof13: cs = 13; goto _test_eof; 
	_test_eof14: cs = 14; goto _test_eof; 
	_test_eof15: cs = 15; goto _test_eof; 
	_test_eof16: cs = 16; goto _test_eof; 
	_test_eof17: cs = 17; goto _test_eof; 
	_test_eof18: cs = 18; goto _test_eof; 
	_test_eof19: cs = 19; goto _test_eof; 
	_test_eof20: cs = 20; goto _test_eof; 
	_test_eof21: cs = 21; goto _test_eof; 
	_test_eof22: cs = 22; goto _test_eof; 
	_test_eof23: cs = 23; goto _test_eof; 
	_test_eof24: cs = 24; goto _test_eof; 
	_test_eof25: cs = 25; goto _test_eof; 
	_test_eof26: cs = 26; goto _test_eof; 
	_test_eof27: cs = 27; goto _test_eof; 
	_test_eof28: cs = 28; goto _test_eof; 
	_test_eof29: cs = 29; goto _test_eof; 
	_test_eof30: cs = 30; goto _test_eof; 
	_test_eof31: cs = 31; goto _test_eof; 
	_test_eof32: cs = 32; goto _test_eof; 
	_test_eof33: cs = 33; goto _test_eof; 
	_test_eof34: cs = 34; goto _test_eof; 
	_test_eof35: cs = 35; goto _test_eof; 
	_test_eof36: cs = 36; goto _test_eof; 
	_test_eof37: cs = 37; goto _test_eof; 
	_test_eof38: cs = 38; goto _test_eof; 
	_test_eof121: cs = 121; goto _test_eof; 
	_test_eof39: cs = 39; goto _test_eof; 
	_test_eof40: cs = 40; goto _test_eof; 
	_test_eof41: cs = 41; goto _test_eof; 
	_test_eof42: cs = 42; goto _test_eof; 
	_test_eof43: cs = 43; goto _test_eof; 
	_test_eof44: cs = 44; goto _test_eof; 
	_test_eof45: cs = 45; goto _test_eof; 
	_test_eof46: cs = 46; goto _test_eof; 
	_test_eof47: cs = 47; goto _test_eof; 
	_test_eof48: cs = 48; goto _test_eof; 
	_test_eof49: cs = 49; goto _test_eof; 
	_test_eof50: cs = 50; goto _test_eof; 
	_test_eof51: cs = 51; goto _test_eof; 
	_test_eof52: cs = 52; goto _test_eof; 
	_test_eof53: cs = 53; goto _test_eof; 
	_test_eof54: cs = 54; goto _test_eof; 
	_test_eof55: cs = 55; goto _test_eof; 
	_test_eof56: cs = 56; goto _test_eof; 
	_test_eof57: cs = 57; goto _test_eof; 
	_test_eof58: cs = 58; goto _test_eof; 
	_test_eof59: cs = 59; goto _test_eof; 
	_test_eof60: cs = 60; goto _test_eof; 
	_test_eof61: cs = 61; goto _test_eof; 
	_test_eof62: cs = 62; goto _test_eof; 
	_test_eof63: cs = 63; goto _test_eof; 
	_test_eof64: cs = 64; goto _test_eof; 
	_test_eof65: cs = 65; goto _test_eof; 
	_test_eof66: cs = 66; goto _test_eof; 
	_test_eof67: cs = 67; goto _test_eof; 
	_test_eof68: cs = 68; goto _test_eof; 
	_test_eof69: cs = 69; goto _test_eof; 
	_test_eof70: cs = 70; goto _test_eof; 
	_test_eof71: cs = 71; goto _test_eof; 
	_test_eof72: cs = 72; goto _test_eof; 
	_test_eof73: cs = 73; goto _test_eof; 
	_test_eof74: cs = 74; goto _test_eof; 
	_test_eof75: cs = 75; goto _test_eof; 
	_test_eof76: cs = 76; goto _test_eof; 
	_test_eof77: cs = 77; goto _test_eof; 
	_test_eof78: cs = 78; goto _test_eof; 
	_test_eof79: cs = 79; goto _test_eof; 
	_test_eof80: cs = 80; goto _test_eof; 
	_test_eof81: cs = 81; goto _test_eof; 
	_test_eof82: cs = 82; goto _test_eof; 
	_test_eof83: cs = 83; goto _test_eof; 
	_test_eof84: cs = 84; goto _test_eof; 
	_test_eof85: cs = 85; goto _test_eof; 
	_test_eof86: cs = 86; goto _test_eof; 
	_test_eof87: cs = 87; goto _test_eof; 
	_test_eof88: cs = 88; goto _test_eof; 
	_test_eof89: cs = 89; goto _test_eof; 
	_test_eof90: cs = 90; goto _test_eof; 
	_test_eof91: cs = 91; goto _test_eof; 
	_test_eof92: cs = 92; goto _test_eof; 
	_test_eof93: cs = 93; goto _test_eof; 
	_test_eof94: cs = 94; goto _test_eof; 
	_test_eof95: cs = 95; goto _test_eof; 
	_test_eof96: cs = 96; goto _test_eof; 
	_test_eof97: cs = 97; goto _test_eof; 
	_test_eof98: cs = 98; goto _test_eof; 
	_test_eof99: cs = 99; goto _test_eof; 
	_test_eof100: cs = 100; goto _test_eof; 
	_test_eof101: cs = 101; goto _test_eof; 
	_test_eof102: cs = 102; goto _test_eof; 
	_test_eof103: cs = 103; goto _test_eof; 
	_test_eof104: cs = 104; goto _test_eof; 
	_test_eof105: cs = 105; goto _test_eof; 
	_test_eof106: cs = 106; goto _test_eof; 
	_test_eof107: cs = 107; goto _test_eof; 
	_test_eof108: cs = 108; goto _test_eof; 
	_test_eof109: cs = 109; goto _test_eof; 
	_test_eof110: cs = 110; goto _test_eof; 
	_test_eof111: cs = 111; goto _test_eof; 
	_test_eof112: cs = 112; goto _test_eof; 
	_test_eof113: cs = 113; goto _test_eof; 
	_test_eof114: cs = 114; goto _test_eof; 
	_test_eof115: cs = 115; goto _test_eof; 
	_test_eof116: cs = 116; goto _test_eof; 
	_test_eof117: cs = 117; goto _test_eof; 
	_test_eof118: cs = 118; goto _test_eof; 
	_test_eof119: cs = 119; goto _test_eof; 
	_test_eof120: cs = 120; goto _test_eof; 

	_test_eof: {}
	if ( p == eof )
	{
	switch ( cs ) {
	case 121: 
#line 162 "util/ipv6_parser.rl"
	{complete_ip();}
	break;
	case 1: 
	case 2: 
	case 3: 
	case 4: 
	case 5: 
	case 6: 
	case 7: 
	case 8: 
	case 9: 
	case 10: 
	case 11: 
	case 12: 
	case 13: 
	case 14: 
	case 15: 
	case 16: 
	case 17: 
	case 18: 
	case 19: 
	case 20: 
	case 21: 
	case 22: 
	case 23: 
	case 24: 
	case 25: 
	case 26: 
	case 27: 
	case 28: 
	case 29: 
	case 30: 
	case 31: 
	case 32: 
	case 33: 
	case 34: 
	case 35: 
	case 36: 
	case 37: 
	case 38: 
	case 39: 
	case 40: 
	case 41: 
	case 42: 
	case 43: 
	case 44: 
	case 45: 
	case 46: 
	case 47: 
	case 48: 
	case 49: 
	case 50: 
	case 51: 
	case 52: 
	case 53: 
	case 54: 
	case 55: 
	case 56: 
	case 57: 
	case 58: 
	case 59: 
	case 60: 
	case 61: 
	case 62: 
	case 63: 
	case 64: 
	case 65: 
	case 66: 
	case 67: 
	case 68: 
	case 69: 
	case 70: 
	case 71: 
	case 72: 
	case 73: 
	case 74: 
	case 75: 
	case 76: 
	case 77: 
	case 78: 
	case 79: 
	case 80: 
	case 81: 
	case 82: 
	case 83: 
	case 84: 
	case 85: 
	case 86: 
	case 87: 
	case 88: 
	case 89: 
	case 90: 
	case 91: 
	case 92: 
	case 93: 
	case 94: 
	case 95: 
	case 96: 
	case 97: 
	case 98: 
	case 99: 
	case 100: 
	case 101: 
	case 102: 
	case 103: 
	case 104: 
	case 105: 
	case 106: 
	case 107: 
	case 108: 
	case 109: 
	case 110: 
	case 111: 
	case 112: 
	case 113: 
	case 114: 
	case 115: 
	case 116: 
	case 117: 
	case 118: 
	case 119: 
	case 120: 
#line 164 "util/ipv6_parser.rl"
	{ parse_error(); }
	break;
#line 2346 "util/ipv6_parser.h"
	}
	}

	_out: {}
	}

#line 246 "util/ipv6_parser.rl"
        // Ragel write exec end
    }

    virtual bool parserError(void)
    {
        return m_error;
    }

    void parse_error(void)
    {
        m_error = true;
    }

    /// Initialize variables in preparation for parsing a new IP address.
    ///
    void start_new_ip(void)
    {
        m_h_front.clear();
        m_h_back.clear();
        m_working_addr.init();
        m_working_addr.setVersion(6);
        m_working_addr.setNetmask(128);
    }


    void complete_ip(void)
    {
        fill_front_and_back();

        if ( (m_working_addr.getNetmask() > 128) ||
             (m_working_addr.getNetmask() < 1) )
        {
            // CIDR Netmask out of range
            parse_error();
        }
    }



    void ipv4_start()
    {
        m_ipv4_start = p; // The variable 'p' is the Ragel pointer to the current character
    }

    void ipv4_end()
    {
        char* start = (char*)m_ipv4_start;
        char* end = NULL;
        long v1;
        long v2;

        v1 = strtol(start, &end, 10);
        if ((v1 < 0) || (v1 > 255))
        {
            m_error = true;
        }

        start = end + 1; // Skip the period
        v2 = strtol(start, &end, 10);
        if ((v2 < 0) || (v2 > 255))
        {
            m_error = true;
        }

        uint16_t v3 = (uint16_t)v1 << 8 | (uint16_t)v2;

        start = end + 1; // Skip the period
        v1 = strtol(start, &end, 10);
        if ((v1 < 0) || (v1 > 255))
        {
            m_error = true;
        }

        start = end + 1; // Skip the period
        v2 = strtol(start, &end, 10);
        if ((v2 < 0) || (v2 > 255))
        {
            m_error = true;
        }

        uint16_t v4 = (uint16_t)v1 << 8 | (uint16_t)v2;

        m_h_back.push_front(v3);
        m_h_back.push_front(v4);

    }


    void h_start()
    {
        m_h_start = p;
    }

    void h_front_push()
    {
        // Convert the hex value and place into m_h_front in the order it is received
        long v = strtol((const char*)m_h_start, NULL, 16);
        //INTZ_DEBUG_ASSERT(v >= 0);
        //INTZ_DEBUG_ASSERT(v <= UINT16_MAX);

        m_h_front.push_back(v);
    }

    void h_back_push()
    {
        // Convert the hex value and place into m_h_back in reverse order from what it is received
        long v = strtol((const char*)m_h_start, NULL, 16);
        //INTZ_DEBUG_ASSERT(v >= 0);
        //INTZ_DEBUG_ASSERT(v <= UINT16_MAX);

        m_h_back.push_front(v);
    }

    void fill_front_and_back(void)
    {
        //INTZ_DEBUG_ASSERT((m_h_front.size() + m_h_back.size()) <= 8);

        for (unsigned int i = 0, j = 0; i < m_h_front.size(); ++i)
        {
            m_working_addr.setByte(m_h_front[i] >> 8, j++);
            m_working_addr.setByte(m_h_front[i] & 0xFF, j++);
        }

        for (unsigned int i = 0, j = 15; i < m_h_back.size(); ++i)
        {
            m_working_addr.setByte(m_h_back[i] & 0xFF, j--);
            m_working_addr.setByte(m_h_back[i] >> 8, j--);
        }
    }

    void prefix_start(void)
    {
        m_prefix_start = p;
    }

    void prefix_end(void)
    {
        uint8_t prefix_value = strtol((const char*)m_prefix_start, NULL, 10);
        m_working_addr.setNetmask(prefix_value);
    }



  protected:

    // The following variables are used during Ragel parsing

    uint8_t*                m_ipv4_start;

    uint8_t*                m_prefix_start;

    uint8_t*                m_h_start;
    std::deque<uint16_t>    m_h_front;
    std::deque<uint16_t>    m_h_back;

    IP_Addr                 m_working_addr;

    // Set to true if an error occurred during parsing
    bool                    m_error;

};




#endif // __IPv6_Parser_RL__

