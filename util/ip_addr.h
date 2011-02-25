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
#include <assert.h>



#define IP_LONG(i) ( ((uint64_t*)m_addr)[i] )
#define IP_WORD(i) ( ((uint32_t*)m_addr)[i] )
#define IP_SHORT(i) ( ((uint16_t*)m_addr)[i] )


class IP_Addr
{
  public:

    IP_Addr() {}

    /*** Create an IPv4 or IPv6 address
     *   @param p_data A pointer to a char[5] or char[17], where the first byte
     *          is the number of bits in the netmask, and the remaining bytes
     *          represent the IP address in network order.
     *   @param p_length Set to 5 for IPv4 or 17 for IPv6
     **/
    IP_Addr(const char* p_data, uint16_t p_length)
    {
        if (p_length == 5)
        {
            m_ip_version = 4;
            m_mask = p_data[0];
            *(uint32_t*)m_addr = *(uint32_t*)&p_data[1];
        }
        else
        if (p_length == 17)
        {
            m_ip_version = 6;
            m_mask = p_data[0];
            *(uint64_t*)&m_addr[0] = *(uint64_t*)&p_data[1];
            *(uint64_t*)&m_addr[8] = *(uint64_t*)&p_data[8+1];
        }
        else
        {
            assert(false);
        }
    }

    /*** Create an IPv4 or IPv6 address
     *   @param p_data A pointer to a char[4] or char[16], where the bytes
     *          represent the IP address in network order.
     *   @param p_netmask The number of bits in the network mask
     *   @param p_length Set to 4 for IPv4 or 16 for IPv6
     **/
    IP_Addr(const char* p_data, uint8_t p_netmask, uint16_t p_length)
    {
        if (p_length == 4)
        {
            m_ip_version = 4;
            m_mask = p_netmask;
            *(uint32_t*)m_addr = *(uint32_t*)&p_data[0];
        }
        else
        if (p_length == 16)
        {
            m_ip_version = 6;
            m_mask = p_netmask;
            *(uint64_t*)&m_addr[0] = *(uint64_t*)&p_data[0];
            *(uint64_t*)&m_addr[8] = *(uint64_t*)&p_data[8];
        }
        else
        {
            assert(false);
        }
    }

    /*** Create an IPv4 address
     *   @param p_ipv4_network IPv4 Address in network order
     *   @param p_netmask Number of bits in the netmask
     **/
    IP_Addr(uint32_t p_ipv4_network, uint8_t p_netmask)
    {
        m_ip_version = 4;
        m_mask = p_netmask;
        *(uint32_t*)m_addr = *(uint32_t*)&p_ipv4_network;
    }

    ~IP_Addr() {}

    // Get Accessors
    uint8_t getVersion(void) const { return m_ip_version; }
    uint8_t getNetmask(void) const { return m_mask; }
    const char* getBinDataPtr(void) const { return (const char*)&m_mask; }
    uint8_t getBinDataLength(void) const
    {
        if (m_ip_version == 4)
            return 5;
        if (m_ip_version == 6)
            return 17;
        else
            assert(false);
    }

    uint8_t getByte(unsigned int i_) const
    {
        assert(i_ >= 0);
        assert(i_ < 16);

        return m_addr[i_];
    }

    uint8_t* getAddress(void) { return m_addr; }
    const uint8_t* getAddress(void) const { return m_addr; }


    // Set Accessors
    void setVersion(uint8_t p_ip_version) { m_ip_version = p_ip_version; }
    void setNetmask(uint8_t p_mask) { m_mask = p_mask; }

    void setZero(void)
    {
        IP_LONG(0) = 0;
        IP_LONG(1) = 0;
    }

    void init(void)
    {
        setZero();
        m_mask = 128;
    }

    void setByte(uint8_t addr_, unsigned int i_)
    {
        assert(i_ >= 0);
        assert(i_ < 16);

        m_addr[i_] = addr_;
    }


    // Category Tests

    bool containsIPv4(void) const
    {
        return (m_ip_version == 6) &&
               (IP_LONG(0) == 0) && (IP_SHORT(4) == 0) && (IP_SHORT(5) == 0xFFFF);
    }


    bool isUnspecified(void) const
    {
        return (m_ip_version == 6) && (IP_LONG(0) == 0) && (IP_LONG(1) == 0);
    }


    bool isLoopback(void) const
    {
        if (m_ip_version == 6)
        {
#ifdef BIG_ENDIAN
            if ((IP_LONG(0) == 0) && (IP_LONG(1) == 0x0000000000000001LL))
                return true;
#else
            if ((IP_LONG(0) == 0) && (IP_LONG(1) == 0x0100000000000000LL))
                return true;
#endif
            return containsIPv4() && (m_addr[12] == 127);
        }

        return false;
    }


    bool isUniqueLocal(void) const
    {
        if (m_ip_version == 6)
            return (m_addr[0] & 0xfe) == 0xfc;

        return false;
    }


    bool isLinkLocal(void) const
    {
        if (m_ip_version == 4)
        {
            return m_addr[0] == 127;
        }
        else
        if (m_ip_version == 6)
        {
#ifdef BIG_ENDIAN
            return (IP_SHORT(0) & 0xffc0) == 0xfe80;
#else
            return (IP_SHORT(0) & 0xc0ff) == 0x80fe;
#endif
        }
        else
        {
            assert(false);
        }

        return false;
    }


    bool isMulticast(void) const
    {
        if (m_ip_version == 4)
        {
            uint8_t ipclass = m_addr[0] >> 4;
            return (ipclass == 0x0E) || (ipclass == 0x0F);
        }
        else
        if (m_ip_version == 6)
        {
            return m_addr[0] == 0xff;
        }
        else
        {
            assert(false);
        }

        return false;
    }

    /// Parse the given IPv4 string into an ip address and optional netmask.
    /// \param p_ipstring A reference to the text of the IPv4 address to convert.
    /// \returns true on a successful parse, false on syntax error.
    ///
    bool parseIPv4(std::string& p_ipstring);
    bool parseIPv4(const char* p_ipstring)
    {
        std::string ipstring = p_ipstring;
        return parseIPv4(ipstring);
    }

    /// Parse the given IPv6 string into an ip address and optional netmask.
    /// \param p_ipstring A reference to the text of the IPv6 address to convert.
    /// \returns true on a successful parse, false on syntax error.
    ///
    bool parseIPv6(std::string& p_ipstring);
    bool parseIPv6(const char* p_ipstring)
    {
        std::string ipstring = p_ipstring;
        return parseIPv6(ipstring);
    }

    // Printing
    std::string print(void) const
    {
        char s[64];

        if (m_ip_version == 4)
        {
            if (m_mask < 32)
            {
                snprintf(s, sizeof(s), "%d.%d.%d.%d/%d",
                    m_addr[0], m_addr[1], m_addr[2], m_addr[3], m_mask);
            }
            else
            {
                snprintf(s, sizeof(s), "%d.%d.%d.%d",
                    m_addr[0], m_addr[1], m_addr[2], m_addr[3]);
            }
        }
        else
        if (m_ip_version == 6)
        {
            if (containsIPv4())
            {
                if (m_mask < 128)
                {
                    snprintf(s, sizeof(s),
                        "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%d.%d.%d.%d/%d",
                        m_addr[0], m_addr[1], m_addr[2], m_addr[3],
                        m_addr[4], m_addr[5], m_addr[6], m_addr[7], 
                        m_addr[8], m_addr[9], m_addr[10], m_addr[11],
                        m_addr[12], m_addr[13], m_addr[14], m_addr[15], m_mask);
                }
                else
                {
                    snprintf(s, sizeof(s),
                        "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%d.%d.%d.%d",
                        m_addr[0], m_addr[1], m_addr[2], m_addr[3],
                        m_addr[4], m_addr[5], m_addr[6], m_addr[7], 
                        m_addr[8], m_addr[9], m_addr[10], m_addr[11],
                        m_addr[12], m_addr[13], m_addr[14], m_addr[15]);
                }
            }
            else
            {
                if (m_mask < 128)
                {
                    snprintf(s, sizeof(s),
                        "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X/%d",
                        m_addr[0], m_addr[1], m_addr[2], m_addr[3],
                        m_addr[4], m_addr[5], m_addr[6], m_addr[7], 
                        m_addr[8], m_addr[9], m_addr[10], m_addr[11],
                        m_addr[12], m_addr[13], m_addr[14], m_addr[15], m_mask);
                }
                else
                {
                    snprintf(s, sizeof(s),
                        "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X",
                        m_addr[0], m_addr[1], m_addr[2], m_addr[3],
                        m_addr[4], m_addr[5], m_addr[6], m_addr[7], 
                        m_addr[8], m_addr[9], m_addr[10], m_addr[11],
                        m_addr[12], m_addr[13], m_addr[14], m_addr[15]);
                }
            }
        }
        else
        {
            assert(false);
        }

        std::string rs = s;
        return rs;
    }


    // Convert the IP address to its broadcast address using the mask.
    // ex: 192.168.1.1/24 -> 192.168.1.256
    IP_Addr broadcast(void) const
    {
        IP_Addr bc = *this;

        if (m_ip_version == 4)
        {
            if (bc.m_mask < 32)
            {
                uint32_t lmask = -1L;
                lmask >>= bc.m_mask;
                int idx = 3;
                while (lmask)
                {
                    bc.m_addr[idx--] |= (uint8_t)lmask;
                    lmask >>= 8;
                }
            }

            bc.m_mask = 32;
        }
        else
        if (m_ip_version == 6)
        {
            if (bc.m_mask < 128)
            {
                int s = bc.m_mask - 64;
                if (s < 0)
                    s = 0;

                uint64_t llmask = -1LL;
                llmask >>= s;
                int idx = 15;
                while (llmask)
                {
                    bc.m_addr[idx--] |= (uint8_t)llmask;
                    llmask >>= 8;
                }
            }

            if (bc.m_mask < 64)
            {
                uint64_t llmask = -1LL;
                llmask >>= bc.m_mask;
                int idx = 7;
                while (llmask)
                {
                    bc.m_addr[idx--] |= (uint8_t)llmask;
                    llmask >>= 8;
                }
            }

            bc.m_mask = 128;
        }
        else
        {
            assert(false);
        }

        return bc;
    }


    // Convert the IP address to its network address using the mask.
    // ex: 192.168.1.1/24 -> 192.168.1.0
    IP_Addr network(void) const
    {
        IP_Addr nw = *this;

        if (m_ip_version == 4)
        {
            if (nw.m_mask < 32)
            {
                uint32_t lmask = -1L;
                lmask >>= nw.m_mask;
                int idx = 3;
                while (lmask)
                {
                    nw.m_addr[idx--] &= ~(uint8_t)lmask;
                    lmask >>= 8;
                }
            }

            nw.m_mask = 32;
        }
        else
        if (m_ip_version == 6)
        {
            if (nw.m_mask < 128)
            {
                int s = nw.m_mask - 64;
                if (s < 0)
                    s = 0;

                uint64_t llmask = -1LL;
                llmask >>= s;
                int idx = 15;
                while (llmask)
                {
                    nw.m_addr[idx--] &= ~(uint8_t)llmask;
                    llmask >>= 8;
                }
            }

            if (m_mask < 64)
            {
                uint64_t llmask = -1LL;
                llmask >>= nw.m_mask;
                int idx = 7;
                while (llmask)
                {
                    nw.m_addr[idx--] &= ~(uint8_t)llmask;
                    llmask >>= 8;
                }
            }

            nw.m_mask = 128;
        }
        else
        {
            assert(false);
        }

        return nw;
    }



    // Comparing
    int compare(const IP_Addr& p_ip) const
    {
        if (m_ip_version != p_ip.m_ip_version)
            return m_ip_version - p_ip.m_ip_version;

        if (m_ip_version == 4)
        {
            if (m_addr[0] > p_ip.m_addr[0]) return 1;
            if (m_addr[0] < p_ip.m_addr[0]) return -1;

            if (m_addr[1] > p_ip.m_addr[1]) return 1;
            if (m_addr[1] < p_ip.m_addr[1]) return -1;

            if (m_addr[2] > p_ip.m_addr[2]) return 1;
            if (m_addr[2] < p_ip.m_addr[2]) return -1;

            if (m_addr[3] > p_ip.m_addr[3]) return 1;
            if (m_addr[3] < p_ip.m_addr[3]) return -1;

            return 0;
        }

        if (m_ip_version == 6)
        {
            return memcmp(m_addr, p_ip.m_addr, sizeof(m_addr));
        }

        assert(false);
        return 0;
    }


    bool contains(const IP_Addr& p_ip) const
    {
        // Test the lower end
        IP_Addr lnw = this->network();
        IP_Addr rnw = p_ip.network();

        if (lnw.compare(rnw) > 0)
            return false;

        // Test the upper end
        IP_Addr lbc = this->broadcast();
        IP_Addr rbc = p_ip.broadcast();

        if (lbc.compare(rbc) < 0)
            return false;

        return true;
    }

    bool within(const IP_Addr& p_ip) const
    {
        // Test the lower end
        IP_Addr lnw = this->network();
        IP_Addr rnw = p_ip.network();

        if (lnw.compare(rnw) < 0)
            return false;

        // Test the upper end
        IP_Addr lbc = this->broadcast();
        IP_Addr rbc = p_ip.broadcast();

        if (lbc.compare(rbc) > 0)
            return false;

        return true;
    }

  protected:

    uint8_t     m_ip_version;

    // The order and size of these must be maintained.
    // This represents the format of the BSON data for an IP address
    struct {
        uint8_t     m_mask;
        uint8_t     m_addr[16]; // Large enough for an IPv6
    };


}; // end class IP_Addr



#endif // _IP_Addr_HPP_

