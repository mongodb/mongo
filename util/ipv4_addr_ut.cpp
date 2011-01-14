
#include <stdio.h>
//#include <string>

#include "ipv4_parser.hpp"

int main(int c, char** v)
{
    // Simple IP
    IPv4_Parser ip1;
    if (!ip1.parse("192.168.1.2"))
        printf("Failed #1000\n");

    if (ip1.m_ip[0] != 192)
        printf("Failed #1010\n");

    if (ip1.m_ip[1] != 168)
        printf("Failed #1020\n");

    if (ip1.m_ip[2] != 1)
        printf("Failed #1030\n");

    if (ip1.m_ip[3] != 2)
        printf("Failed #1040\n");

    if (ip1.m_mask != 32)
        printf("Failed #1050\n");

    // Malformed Simple IPs
    IPv4_Parser ip2;
    if (ip2.parse("192"))
        printf("Failed #1110\n");

    if (ip2.parse("192."))
        printf("Failed #1115\n");

    if (ip2.parse("192.168"))
        printf("Failed #1120\n");

    if (ip2.parse("192.168."))
        printf("Failed #1125\n");

    if (ip2.parse("192.168.1"))
        printf("Failed #1130\n");

    if (ip2.parse("192.168.1."))
        printf("Failed #1135\n");

    if (ip2.parse("192.168.1.2 "))
        printf("Failed #1140\n");

    // Out-of-bounds checks
    if (ip2.parse("256.1.1.1"))
        printf("Failed #1200\n");

    if (ip2.parse("1.256.1.1"))
        printf("Failed #1205\n");

    if (ip2.parse("1.1.256.1"))
        printf("Failed #1210\n");

    if (ip2.parse("1.1.1.256"))
        printf("Failed #1215\n");

    if (!ip2.parse("0.0.0.0"))
        printf("Failed #1220\n");


    // Test Class A Netmasks
    if (!ip1.parse("1/8"))
        printf("Failed #2000\n");

    if (ip1.m_mask != 8)
        printf("Failed #2005\n");

    if (!ip1.parse("1.2/8"))
        printf("Failed #2015\n");

    if (!ip1.parse("1.2.3/8"))
        printf("Failed #2020\n");

    if (!ip1.parse("1.2.3.4/8"))
        printf("Failed #2025\n");

    if (!ip1.parse("1.2/16"))
        printf("Failed #2100\n");

    if (ip1.m_mask != 16)
        printf("Failed #2105\n");

    if (!ip1.parse("192.168.0.0/16"))
        printf("Failed #2127\n");

    if (!ip1.parse("1.2.3/16"))
        printf("Failed #2120\n");

    if (!ip1.parse("1.2.3.4/16"))
        printf("Failed #2125\n");


    if (!ip1.parse("63/8"))
        printf("Failed #2200\n");



    // Odd but legal Class A Netmasks
    if (!ip1.parse("1./8"))
        printf("Failed #2400\n");

    if (!ip1.parse("1.2./8"))
        printf("Failed #2405\n");

    if (!ip1.parse("1.2.3./8"))
        printf("Failed #2410\n");

    if (!ip1.parse("1.2.3.4/32"))
        printf("Failed #2420\n");


    // Malformed Class A Netmasks

    if (ip1.parse("1.2.3./7"))
        printf("Failed #2505\n");

    if (ip1.parse("1.2.3./0"))
        printf("Failed #2510\n");

    if (ip1.parse("1.2.3./33"))
        printf("Failed #2515\n");


    // Test Class B Netmasks
    if (!ip1.parse("64.2/16"))
        printf("Failed #3000\n");

    if (!ip1.parse("64.2.3/16"))
        printf("Failed #3020\n");

    if (!ip1.parse("64.2.3.4/16"))
        printf("Failed #3025\n");

    if (!ip1.parse("64.2.3/24"))
        printf("Failed #3100\n");

    if (!ip1.parse("64.2.3.4/24"))
        printf("Failed #3125\n");

    if (!ip1.parse("191.1/16"))
        printf("Failed #3200\n");



    // Test Class C Netmasks
    if (!ip1.parse("192.2.3/24"))
        printf("Failed #4000\n");

    if (!ip1.parse("192.2.3.4/24"))
        printf("Failed #4025\n");

    if (!ip1.parse("223.1.2/24"))
        printf("Failed #4200\n");



    // Other invalid forms
    if (ip1.parse(""))
        printf("Failed #7000\n");

    if (ip1.parse(" "))
        printf("Failed #7010\n");


    printf("Tests Complete\n");
    return 0;
}

