#ifndef _IP_Addr_HPP_
#define _IP_Addr_HPP_

/*
 *    Copyright 2011 Intrusion Inc
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>




class IP_Addr
{
  public:

    IP_Addr() {}
    virtual ~IP_Addr() {}

    // Get Accessors
    uint8_t getNetmask(void) const { return m_mask; }
    uint8_t getVersion(void) const { return m_ip_version; }

    // Set Accessors
    void setNetmask(uint8_t p_mask) { m_mask = p_mask; }
    virtual void setZero(void) {}
    virtual void init(void) {}

    // Printing
    virtual std::string print(void) const
    {
        std::string err = "Invalid";
        return err;
    }


  protected:

    uint8_t     m_mask;
    uint8_t     m_ip_version;

}; // end class IP_Addr




class IPv4_Addr : public IP_Addr
{
  public:

    IPv4_Addr()
    {
        m_ip_version = 4;
    }

    virtual void setZero(void)
    {
        m_long = 0;
    }

    virtual void init(void)
    {
        setZero();
        m_mask = 32;
    };

    /// Parse the given string into an ip address and optional netmask.
    /// \param p_ipstring A reference to the text of the IP address to convert.
    /// \returns true on a successful parse, false on syntax error.
    ///
    bool parse(std::string& p_ipstring);


    virtual std::string print(void) const
    {
        char s[24];

        if (m_mask < 32)
        {
            snprintf(s, sizeof(s), "%d.%d.%d.%d/%d", m_ip[0], m_ip[1], m_ip[2], m_ip[3], m_mask);
        }
        else
        {
            snprintf(s, sizeof(s), "%d.%d.%d.%d", m_ip[0], m_ip[1], m_ip[2], m_ip[3]);
        }

        std::string rs = s;
        return rs;
    }


  private:

    union {
        uint8_t     m_ip[4];
        uint32_t    m_long;
    };

}; // end class IPv4_Addr





class IPv6_Addr : public IP_Addr
{
  public:

    IPv6_Addr()
    {
        m_ip_version = 6;
    }

    virtual ~IPv6_Addr() { }

    // Get Accessors

    uint8_t getByte(unsigned int i_) const
    {
        //INTZ_DEBUG_ASSERT(i_ >= 0);
        //INTZ_DEBUG_ASSERT(i_ < 16);

        return _byte[i_];
    }

    uint8_t* getAddress(void) { return _byte; }

    const uint8_t* getAddress(void) const { return _byte; }


    // Set Accessors

    virtual void setZero(void)
    {
        _long[0] = 0;
        _long[1] = 0;
    }

    virtual void init(void)
    {
        setZero();
        m_mask = 128;
    }

    void setNetwork(uint8_t* addr_)
    {
        memcpy(_byte, addr_, sizeof(*this));
    }


    void setByte(uint8_t addr_, unsigned int i_)
    {
        //INTZ_DEBUG_ASSERT(i_ >= 0);
        //INTZ_DEBUG_ASSERT(i_ < 16);

        _byte[i_] = addr_;
    }


    void setNetmask(uint8_t mask_)
    {
        m_mask = mask_;
    }


    void setAddressIPv4Network(uint32_t addr_)
    {
        _long[0] = 0;

#ifdef BIG_ENDIAN
        _word[2] = 0x0000FFFF;
#else
        _word[2] = 0xFFFF0000;
#endif

        _word[3] = addr_;
    }


    bool containsIPv4(void) const
    {
#ifdef BIG_ENDIAN
        return (_long[0] == 0) && (_word[2] == 0x0000FFFF);
#else
        return (_long[0] == 0) && (_word[2] == 0xFFFF0000);
#endif
    }


    bool isUnspecified(void) const
    {
        return (_long[0] == 0) && (_long[1] == 0);
    }


    bool isLoopback(void) const
    {
#ifdef BIG_ENDIAN
        if ((_long[0] == 0) && (_long[1] == 0x0000000000000001LL))
            return true;
#else
        if ((_long[0] == 0) && (_long[1] == 0x0100000000000000LL))
            return true;
#endif
        return containsIPv4() && (_byte[12] == 127);
    }


    bool isUniqueLocal(void) const
    {
        return (_byte[0] & 0xfe) == 0xfc;
    }


    bool isLinkLocal(void) const
    {
#ifdef BIG_ENDIAN
        return (_short[0] & 0xffc0) == 0xfe80;
#else
        return (_short[0] & 0xc0ff) == 0x80fe;
#endif
    }


    bool isMulticast(void) const
    {
        return _byte[0] == 0xff;
    }


    /// Parse the given string into an ip address and optional netmask.
    /// \param p_ipstring A reference to the text of the IP address to convert.
    /// \returns true on a successful parse, false on syntax error.
    ///
    bool parse(std::string& p_ipstring);


    virtual std::string print(void) const
    {
        char s[64];

        if (containsIPv4())
        {
            if (m_mask < 128)
            {
                snprintf(s, sizeof(s),
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%d.%d.%d.%d/%d",
                    _byte[0], _byte[1], _byte[2], _byte[3],
                    _byte[4], _byte[5], _byte[6], _byte[7], 
                    _byte[8], _byte[9], _byte[10], _byte[11],
                    _byte[12], _byte[13], _byte[14], _byte[15], m_mask);
            }
            else
            {
                snprintf(s, sizeof(s),
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%d.%d.%d.%d",
                    _byte[0], _byte[1], _byte[2], _byte[3],
                    _byte[4], _byte[5], _byte[6], _byte[7], 
                    _byte[8], _byte[9], _byte[10], _byte[11],
                    _byte[12], _byte[13], _byte[14], _byte[15]);
            }
        }
        else
        {
            if (m_mask < 128)
            {
                snprintf(s, sizeof(s),
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x/%d",
                    _byte[0], _byte[1], _byte[2], _byte[3],
                    _byte[4], _byte[5], _byte[6], _byte[7], 
                    _byte[8], _byte[9], _byte[10], _byte[11],
                    _byte[12], _byte[13], _byte[14], _byte[15], m_mask);
            }
            else
            {
                snprintf(s, sizeof(s),
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                    _byte[0], _byte[1], _byte[2], _byte[3],
                    _byte[4], _byte[5], _byte[6], _byte[7], 
                    _byte[8], _byte[9], _byte[10], _byte[11],
                    _byte[12], _byte[13], _byte[14], _byte[15]);
            }
        }

        std::string rs = s;
        return rs;
    }


  protected:

    // Warning: do not add any other non-static data items to this class!
    // Please note that the address is stored in Network Order.

    union {

        // IPv6 addresses are 128 bits long!
        uint64_t           _long[2]; 
        uint32_t           _word[4]; 
        uint16_t           _short[4*2];
        uint8_t            _byte[4*4];

    };

}; // end class IPv6_Addr


inline bool operator<(const IPv6_Addr& p_a, const IPv6_Addr& p_b)
{
    
    // but that would not compare correctly since _long[]
    // is stored in network order.

    if (p_a.getByte(0) < p_b.getByte(0)) return true;
    if (p_a.getByte(0) > p_b.getByte(0)) return false;

    if (p_a.getByte(1) < p_b.getByte(1)) return true;
    if (p_a.getByte(1) > p_b.getByte(1)) return false;

    if (p_a.getByte(2) < p_b.getByte(2)) return true;
    if (p_a.getByte(2) > p_b.getByte(2)) return false;

    if (p_a.getByte(3) < p_b.getByte(3)) return true;
    if (p_a.getByte(3) > p_b.getByte(3)) return false;

    if (p_a.getByte(4) < p_b.getByte(4)) return true;
    if (p_a.getByte(4) > p_b.getByte(4)) return false;

    if (p_a.getByte(5) < p_b.getByte(5)) return true;
    if (p_a.getByte(5) > p_b.getByte(5)) return false;

    if (p_a.getByte(6) < p_b.getByte(6)) return true;
    if (p_a.getByte(6) > p_b.getByte(6)) return false;

    if (p_a.getByte(7) < p_b.getByte(7)) return true;
    if (p_a.getByte(7) > p_b.getByte(7)) return false;

    if (p_a.getByte(8) < p_b.getByte(8)) return true;
    if (p_a.getByte(8) > p_b.getByte(8)) return false;

    if (p_a.getByte(9) < p_b.getByte(9)) return true;
    if (p_a.getByte(9) > p_b.getByte(9)) return false;

    if (p_a.getByte(10) < p_b.getByte(10)) return true;
    if (p_a.getByte(10) > p_b.getByte(10)) return false;

    if (p_a.getByte(11) < p_b.getByte(11)) return true;
    if (p_a.getByte(11) > p_b.getByte(11)) return false;

    if (p_a.getByte(12) < p_b.getByte(12)) return true;
    if (p_a.getByte(12) > p_b.getByte(12)) return false;

    if (p_a.getByte(13) < p_b.getByte(13)) return true;
    if (p_a.getByte(13) > p_b.getByte(13)) return false;

    if (p_a.getByte(14) < p_b.getByte(14)) return true;
    if (p_a.getByte(14) > p_b.getByte(14)) return false;

    if (p_a.getByte(15) < p_b.getByte(15)) return true;

    return false;
}



inline bool operator>(const IPv6_Addr& p_a, const IPv6_Addr& p_b)
{
    if (p_a.getByte(0) > p_b.getByte(0)) return true;
    if (p_a.getByte(0) < p_b.getByte(0)) return false;

    if (p_a.getByte(1) > p_b.getByte(1)) return true;
    if (p_a.getByte(1) < p_b.getByte(1)) return false;

    if (p_a.getByte(2) > p_b.getByte(2)) return true;
    if (p_a.getByte(2) < p_b.getByte(2)) return false;

    if (p_a.getByte(3) > p_b.getByte(3)) return true;
    if (p_a.getByte(3) < p_b.getByte(3)) return false;

    if (p_a.getByte(4) > p_b.getByte(4)) return true;
    if (p_a.getByte(4) < p_b.getByte(4)) return false;

    if (p_a.getByte(5) > p_b.getByte(5)) return true;
    if (p_a.getByte(5) < p_b.getByte(5)) return false;

    if (p_a.getByte(6) > p_b.getByte(6)) return true;
    if (p_a.getByte(6) < p_b.getByte(6)) return false;

    if (p_a.getByte(7) > p_b.getByte(7)) return true;
    if (p_a.getByte(7) < p_b.getByte(7)) return false;

    if (p_a.getByte(8) > p_b.getByte(8)) return true;
    if (p_a.getByte(8) < p_b.getByte(8)) return false;

    if (p_a.getByte(9) > p_b.getByte(9)) return true;
    if (p_a.getByte(9) < p_b.getByte(9)) return false;

    if (p_a.getByte(10) > p_b.getByte(10)) return true;
    if (p_a.getByte(10) < p_b.getByte(10)) return false;

    if (p_a.getByte(11) > p_b.getByte(11)) return true;
    if (p_a.getByte(11) < p_b.getByte(11)) return false;

    if (p_a.getByte(12) > p_b.getByte(12)) return true;
    if (p_a.getByte(12) < p_b.getByte(12)) return false;

    if (p_a.getByte(13) > p_b.getByte(13)) return true;
    if (p_a.getByte(13) < p_b.getByte(13)) return false;

    if (p_a.getByte(14) > p_b.getByte(14)) return true;
    if (p_a.getByte(14) < p_b.getByte(14)) return false;

    if (p_a.getByte(15) > p_b.getByte(15)) return true;


    return false;
}

#include "ipv6_parser.h"

inline bool IPv6_Addr::parse(std::string& p_ipstring)
{
    IPv6_Parser ipp;
    return ipp.parse(p_ipstring);
}

#endif // _IP_Addr_HPP_

