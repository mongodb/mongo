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


%%{
    machine ipv6;
    alphtype unsigned char;


    # Define an IPv4 address
    IPV4 = (digit{1,3} '.' digit{1,3} '.' digit{1,3} '.' digit{1,3}) >{ ipv4_start(); } %{ ipv4_end(); };

    # Values pushed from the front of the address (before the '::')
    HF = xdigit{1,4} >{ h_start(); } %{ h_front_push(); };

    # Values pushed from the back of the address (after the '::')
    HB = xdigit{1,4} >{ h_start(); } %{ h_back_push(); };

    PREFIX = '/' (digit{1,3} >{ prefix_start(); } %{ prefix_end(); });

    # Define the various Forms of IPv6 addresses

    F10 = HF ':' HF ':' HF ':' HF ':' HF ':' HF ':' HF ':' HF;

    F11 = HF '::' HB ':' HB ':' HB ':' HB ':' HB ':' HB;
    F12 = HF '::' HB ':' HB ':' HB ':' HB ':' HB;
    F13 = HF '::' HB ':' HB ':' HB ':' HB;
    F14 = HF '::' HB ':' HB ':' HB;
    F15 = HF '::' HB ':' HB;
    F16 = HF '::' HB;
    F17 = HF '::';

    F21 = HF ':' HF '::' HB ':' HB ':' HB ':' HB ':' HB;
    F22 = HF ':' HF '::' HB ':' HB ':' HB ':' HB;
    F23 = HF ':' HF '::' HB ':' HB ':' HB;
    F24 = HF ':' HF '::' HB ':' HB;
    F25 = HF ':' HF '::' HB;
    F26 = HF ':' HF '::';

    F31 = HF ':' HF ':' HF '::' HB ':' HB ':' HB ':' HB;
    F32 = HF ':' HF ':' HF '::' HB ':' HB ':' HB;
    F33 = HF ':' HF ':' HF '::' HB ':' HB;
    F34 = HF ':' HF ':' HF '::' HB;
    F35 = HF ':' HF ':' HF '::';

    F41 = HF ':' HF ':' HF ':' HF '::' HB ':' HB ':' HB;
    F42 = HF ':' HF ':' HF ':' HF '::' HB ':' HB;
    F43 = HF ':' HF ':' HF ':' HF '::' HB;
    F44 = HF ':' HF ':' HF ':' HF '::';

    F51 = HF ':' HF ':' HF ':' HF ':' HF '::' HB ':' HB;
    F52 = HF ':' HF ':' HF ':' HF ':' HF '::' HB;
    F53 = HF ':' HF ':' HF ':' HF ':' HF '::';

    F61 = HF ':' HF ':' HF ':' HF ':' HF ':' HF '::' HB;
    F62 = HF ':' HF ':' HF ':' HF ':' HF ':' HF '::';

    F71 = HF ':' HF ':' HF ':' HF ':' HF ':' HF ':' HF '::';

    F81 = '::' HB ':' HB ':' HB ':' HB ':' HB ':' HB ':' HB;
    F82 = '::' HB ':' HB ':' HB ':' HB ':' HB ':' HB;
    F83 = '::' HB ':' HB ':' HB ':' HB ':' HB;
    F84 = '::' HB ':' HB ':' HB ':' HB;
    F85 = '::' HB ':' HB ':' HB;
    F86 = '::' HB ':' HB;
    F87 = '::' HB;

    F91 = '::';


    # Define Forms that end with an IPv4 Address

    F100 = HF ':' HF ':' HF ':' HF ':' HF ':' HF ':' IPV4;

    F111 = HF '::' HB ':' HB ':' HB ':' HB ':' IPV4;
    F112 = HF '::' HB ':' HB ':' HB ':' IPV4;
    F113 = HF '::' HB ':' HB ':' IPV4;
    F114 = HF '::' HB ':' IPV4;
    F115 = HF '::' IPV4;

    F121 = HF ':' HF '::' HB ':' HB ':' HB ':' IPV4;
    F122 = HF ':' HF '::' HB ':' HB ':' IPV4;
    F123 = HF ':' HF '::' HB ':' IPV4;
    F124 = HF ':' HF '::' IPV4;

    F131 = HF ':' HF ':' HF '::' HB ':' HB ':' IPV4;
    F132 = HF ':' HF ':' HF '::' HB ':' IPV4;
    F133 = HF ':' HF ':' HF '::' IPV4;

    F141 = HF ':' HF ':' HF ':' HF '::' HB ':' IPV4;
    F142 = HF ':' HF ':' HF ':' HF '::' IPV4;

    F151 = HF ':' HF ':' HF ':' HF ':' HF '::' IPV4;

    F161 = '::' HB ':' HB ':' HB ':' HB ':' HB ':' IPV4;
    F162 = '::' HB ':' HB ':' HB ':' HB ':' IPV4;
    F163 = '::' HB ':' HB ':' HB ':' IPV4;
    F164 = '::' HB ':' HB ':' IPV4;
    F165 = '::' HB ':' IPV4;

    F171 = '::' IPV4;


    # Bring the various forms together

    IPV6_ONLY = F10 | F11 | F12 | F13 | F14 | F15 | F16 | F17 |
                F21 | F22 | F23 | F24 | F25 | F26 |
                F31 | F32 | F33 | F34 | F35 |
                F41 | F42 | F43 | F44 |
                F51 | F52 | F53 |
                F61 | F62 |
                F71 |
                F81 | F82 | F83 | F84 | F85 | F86 | F87 |
                F91;

    IPV6_V4 = F100 | F111 | F112 | F113 | F114 | F115 | 
              F121 | F122 | F123 | F124 |
              F131 | F132 | F133 |
              F141 | F142 |
              F151 |
              F161 | F162 | F163 | F164 | F165 |
              F171;

    IPV6 = IPV6_ONLY | IPV6_V4;

    IPV6_NETMASK = IPV6 PREFIX?;

    ipv6 = IPV6_NETMASK ('\0' %{complete_ip();}) ;

    main := ipv6 $err{ parse_error(); };


    prepush {
        // prepush
        // Make sure the stack will not overflow
        // This will ruin the outcome of the machine,
        // but will prevent stepping outside the stack.
        if (act >= (m_max_stack - 1))
            act = m_max_stack - 2;
    }

    postpop {
        // postpop - Nothing to do
    }

}%%


// Ragel Table Data
%% write data;



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

        %% write init;
    }

    void ragel_exec(void)
    {
        // Ragel write exec begin
        %% write exec;
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

