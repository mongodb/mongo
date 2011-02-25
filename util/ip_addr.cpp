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

#include "ip_addr.h"
#include "ipv6_parser.h"


bool IP_Addr::parseIPv4(std::string& p_ipstring)
{
    typedef enum
    {

        PARSE_START,
        PARSE_FIELD1,
        PARSE_F1_NETMASK_START,
        PARSE_F1_NETMASK,
        PARSE_FIELD2_START,
        PARSE_FIELD2,
        PARSE_F2_NETMASK_START,
        PARSE_F2_NETMASK,
        PARSE_FIELD3_START,
        PARSE_FIELD3,
        PARSE_F3_NETMASK_START,
        PARSE_F3_NETMASK,
        PARSE_FIELD4_START,
        PARSE_FIELD4,
        PARSE_F4_NETMASK_START,
        PARSE_F4_NETMASK

    } IPv4_Addr_States_e;

    int state = PARSE_START;

    // Initialize the internal variables
    init();

    // Assume IPv4
    setVersion(4);
    setNetmask(32);

    // Copy the string so we can modify it
    char scopy[p_ipstring.length() + 2];
    strcpy(scopy, p_ipstring.c_str());
    char* sptr = scopy;
    char* field_ptr = scopy;

    while (true)
    { 
        int val;
        int c = *sptr;

        switch (state)
        {
            case PARSE_START:
                state = PARSE_FIELD1;


            // **** Field 1 ****

            case PARSE_FIELD1:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '.')
                {
                    // Field 1 contained a single octet
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[0] = val;
                    state = PARSE_FIELD2_START;
                    break;
                }

                if (c == '/')
                {
                    // Field 1 is terminated by a netmask
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[0] = val;
                    state = PARSE_F1_NETMASK_START;
                    break;
                }

                // Reject all other input chars
                return false;


            case PARSE_F1_NETMASK_START:
                field_ptr = sptr;
                state = PARSE_F1_NETMASK;

            case PARSE_F1_NETMASK:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '\0')
                {
                    // Fail if this is not a class A address
                    if (m_addr[0] & 0x80)
                        return false;

                    m_mask = atoi(field_ptr);

                    // Fail on impossible netmask values
                    if (m_mask != 8)
                        return false;

                    return true;
                }

                // Reject all other input chars
                return false;



            // **** Field 2 ****

            case PARSE_FIELD2_START:
                field_ptr = sptr;
                state = PARSE_FIELD2;


            case PARSE_FIELD2:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '.')
                {
                    // Field 2 contained a single octet
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[1] = val;
                    state = PARSE_FIELD3_START;
                    break;
                }

                if (c == '/')
                {
                    // Field 2 is terminated by a netmask
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_mask = val;
                    state = PARSE_F2_NETMASK_START;
                    break;
                }

                // Reject all other input chars
                return false;

            case PARSE_F2_NETMASK_START:
                field_ptr = sptr;
                state = PARSE_F2_NETMASK;

            case PARSE_F2_NETMASK:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '\0')
                {
                    // Fail if this is not a class A or B address
                    if ((m_addr[0] & 0xF0) >= 0xC0)
                        return false;

                    val = atoi(field_ptr);

                    // Fail on impossible netmask values
                    if ((val < 8) || (val > 16))
                        return false;

                    // Fail Class B address with undersized netmask
                    if ((val < 16) && ((m_addr[0] & 0xC0) == 0x80))
                            return false;

                    m_mask = val;

                    return true;
                }

                // Reject all other input chars
                return false;


            // **** Field 3 ****

            case PARSE_FIELD3_START:
                field_ptr = sptr;
                state = PARSE_FIELD3;


            case PARSE_FIELD3:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '.')
                {
                    // Field 3 contained a single octet
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[2] = val;
                    state = PARSE_FIELD4_START;
                    break;
                }

                if (c == '/')
                {
                    // Field 3 is terminated by a netmask
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[2] = val;
                    state = PARSE_F3_NETMASK_START;
                    break;
                }

                // Reject all other input chars
                return false;

            case PARSE_F3_NETMASK_START:
                field_ptr = sptr;
                state = PARSE_F3_NETMASK;

            case PARSE_F3_NETMASK:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '\0')
                {
                    val = atoi(field_ptr);

                    // Fail on impossible netmask values
                    if ((val < 8) || (val > 24))
                        return false;

                    // Fail Class B address with undersized netmask
                    if ((val < 16) && ((m_addr[0] & 0xC0) == 0x80))
                            return false;

                    m_mask = val;

                    return true;
                }

                // Reject all other input chars
                return false;


            // **** Field 4 ****

            case PARSE_FIELD4_START:
                field_ptr = sptr;
                state = PARSE_FIELD4;


            case PARSE_FIELD4:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '\0')
                {
                    if (field_ptr == sptr)
                        return false;

                    // Field 4 contained a single octet
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[3] = val;
                    return true;
                }

                if (c == '/')
                {
                    // Field 4 is terminated by a netmask
                    *sptr++ = '\0';
                    val = atoi(field_ptr);
                    if ((val < 0) || (val > 255))
                        return false;

                    m_addr[3] = val;
                    state = PARSE_F4_NETMASK_START;
                    break;
                }

                // Reject all other input chars
                return false;

            case PARSE_F4_NETMASK_START:
                field_ptr = sptr;
                state = PARSE_F4_NETMASK;

            case PARSE_F4_NETMASK:

                if (isdigit(c))
                {
                    ++sptr;
                    break;
                }

                if (c == '\0')
                {
                    val = atoi(field_ptr);

                    // Fail on impossible netmask values
                    if ((val < 8) || (val > 32))
                        return false;

                    m_mask = val;

                    return true;
                }

                // Reject all other input chars
                return false;


            default:
                // Invalid state
                return false;

        } // end switch

    } // end while

    return false;
}


bool IP_Addr::parseIPv6(std::string& p_ipstring)
{
    IPv6_Parser ipp;
    if (!ipp.parse(p_ipstring))
        return false;

    *this = ipp.getIPv6();
    return true;
}

