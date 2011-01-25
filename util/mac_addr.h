#ifndef _MAC_Addr_HPP_
#define _MAC_Addr_HPP_

/*
 *    Copyright 2005-2011 Intrusion Inc
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




class MAC_Addr
{
  public:

    MAC_Addr()
    {
    }

    void setZero(void)
    {
        m_addr[0] = 0;
        m_addr[1] = 0;
        m_addr[2] = 0;
        m_addr[3] = 0;
        m_addr[4] = 0;
        m_addr[5] = 0;
    }

    /// Parse the given string into an ip address and optional netmask.
    /// \param p_macstr A string containing text of the MAC address to convert.
    /// \returns true on a successful parse, false on syntax error.
    ///
    bool parse(std::string p_macstr)
    {
        p_macstr += ":";
        const char* sptr = p_macstr.c_str();
        int val;
        char* eptr;

        for (int i = 0; i < 6; ++i)
        {
            val = strtol(sptr, &eptr, 16);
            if ((val < 0) || (val > 255))
                return false;

            // No progress?
            if (sptr == eptr)
                return false;

            // Skip over colon
            sptr = eptr + 1;
            m_addr[i] = val;
        }

        return true;
    }


    std::string print(void) const
    {
        char s[24];

        snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
            m_addr[0], m_addr[1], m_addr[2],
            m_addr[3], m_addr[4], m_addr[5]);

        std::string rs = s;
        return rs;
    }


  private:

    uint8_t     m_addr[6];

}; // end class MAC_Addr




#endif // _MAC_Addr_HPP_

