
#include <stdio.h>

#include "ipv6_parser.h"

#define ASSERT(a, t) if (!(a)) printf("Failed %d\n", t);

int main(int c, char** v)
{
    IPv6_Parser ipp1;
    IPv6_Addr ip1;

    // Simple IP
    ASSERT(ipp1.parse("::192.168.1.2"), 1000);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0, 1010);
    ASSERT(ip1.getByte(1) == 0, 1020);
    ASSERT(ip1.getByte(2) == 0, 1030);
    ASSERT(ip1.getByte(3) == 0, 1040);
    ASSERT(ip1.getByte(4) == 0, 1050);
    ASSERT(ip1.getByte(5) == 0, 1060);
    ASSERT(ip1.getByte(6) == 0, 1070);
    ASSERT(ip1.getByte(7) == 0, 1080);
    ASSERT(ip1.getByte(8) == 0, 1090);
    ASSERT(ip1.getByte(9) == 0, 1100);
    ASSERT(ip1.getByte(10) == 0, 1110);
    ASSERT(ip1.getByte(11) == 0, 1120);
    ASSERT(ip1.getByte(12) == 192, 1130);
    ASSERT(ip1.getByte(13) == 168, 1140);
    ASSERT(ip1.getByte(14) == 1, 1150);
    ASSERT(ip1.getByte(15) == 2, 1160);

    ASSERT(ipp1.parse("1234::192.168.1.2"), 1200);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0x12, 1210);
    ASSERT(ip1.getByte(1) == 0x34, 1220);
    ASSERT(ip1.getByte(2) == 0, 1230);
    ASSERT(ip1.getByte(3) == 0, 1240);
    ASSERT(ip1.getByte(4) == 0, 1250);
    ASSERT(ip1.getByte(5) == 0, 1260);
    ASSERT(ip1.getByte(6) == 0, 1270);
    ASSERT(ip1.getByte(7) == 0, 1280);
    ASSERT(ip1.getByte(8) == 0, 1290);
    ASSERT(ip1.getByte(9) == 0, 1300);
    ASSERT(ip1.getByte(10) == 0, 1310);
    ASSERT(ip1.getByte(11) == 0, 1320);
    ASSERT(ip1.getByte(12) == 192, 1330);
    ASSERT(ip1.getByte(13) == 168, 1340);
    ASSERT(ip1.getByte(14) == 1, 1350);
    ASSERT(ip1.getByte(15) == 2, 1360);


    // Malformed Simple IPs
    ASSERT(!ipp1.parse("1234::192."), 1400);
    ASSERT(!ipp1.parse("::192.168"), 1410);
    ASSERT(!ipp1.parse("::192.168."), 1420);
    ASSERT(!ipp1.parse("::192.168.1"), 1430);
    ASSERT(!ipp1.parse("::192.168.1."), 1440);

    // Out-of-bounds checks
    ASSERT(!ipp1.parse("::255.1.1.1/129"), 1500);
    ASSERT(!ipp1.parse("::ABCDE"), 1510);
    ASSERT(!ipp1.parse("1:2:3:4:5:6:7:8:9"), 1520);
    ASSERT(!ipp1.parse("1::2::3"), 1530);


    // Test Zero Compression
    ASSERT(ipp1.parse("1234::"), 1600);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0x12, 1610);
    ASSERT(ip1.getByte(1) == 0x34, 1620);
    ASSERT(ip1.getByte(2) == 0, 1630);
    ASSERT(ip1.getByte(3) == 0, 1640);
    ASSERT(ip1.getByte(4) == 0, 1650);
    ASSERT(ip1.getByte(5) == 0, 1660);
    ASSERT(ip1.getByte(6) == 0, 1670);
    ASSERT(ip1.getByte(7) == 0, 1680);
    ASSERT(ip1.getByte(8) == 0, 1690);
    ASSERT(ip1.getByte(9) == 0, 1700);
    ASSERT(ip1.getByte(10) == 0, 1710);
    ASSERT(ip1.getByte(11) == 0, 1720);
    ASSERT(ip1.getByte(12) == 0, 1730);
    ASSERT(ip1.getByte(13) == 0, 1740);
    ASSERT(ip1.getByte(14) == 0, 1750);
    ASSERT(ip1.getByte(15) == 0, 1760);

    ASSERT(ipp1.parse("::1234"), 1800);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0, 1810);
    ASSERT(ip1.getByte(1) == 0, 1820);
    ASSERT(ip1.getByte(2) == 0, 1830);
    ASSERT(ip1.getByte(3) == 0, 1840);
    ASSERT(ip1.getByte(4) == 0, 1850);
    ASSERT(ip1.getByte(5) == 0, 1860);
    ASSERT(ip1.getByte(6) == 0, 1870);
    ASSERT(ip1.getByte(7) == 0, 1880);
    ASSERT(ip1.getByte(8) == 0, 1890);
    ASSERT(ip1.getByte(9) == 0, 1900);
    ASSERT(ip1.getByte(10) == 0, 1910);
    ASSERT(ip1.getByte(11) == 0, 1920);
    ASSERT(ip1.getByte(12) == 0, 1930);
    ASSERT(ip1.getByte(13) == 0, 1940);
    ASSERT(ip1.getByte(14) == 0x12, 1950);
    ASSERT(ip1.getByte(15) == 0x34, 1960);


    // Test Netmasks
    ASSERT(ipp1.parse("1::/8"), 2000);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0, 2010);
    ASSERT(ip1.getByte(1) == 1, 2020);
    ASSERT(ip1.getByte(2) == 0, 2030);
    ASSERT(ip1.getByte(3) == 0, 2040);
    ASSERT(ip1.getByte(4) == 0, 2050);
    ASSERT(ip1.getByte(5) == 0, 2060);
    ASSERT(ip1.getByte(6) == 0, 2070);
    ASSERT(ip1.getByte(7) == 0, 2080);
    ASSERT(ip1.getByte(8) == 0, 2090);
    ASSERT(ip1.getByte(9) == 0, 2100);
    ASSERT(ip1.getByte(10) == 0, 2110);
    ASSERT(ip1.getByte(11) == 0, 2120);
    ASSERT(ip1.getByte(12) == 0, 2130);
    ASSERT(ip1.getByte(13) == 0, 2140);
    ASSERT(ip1.getByte(14) == 0, 2150);
    ASSERT(ip1.getByte(15) == 0, 2160);
    ASSERT(ip1.getNetmask() == 8, 2170);


    ASSERT(ipp1.parse("1234:5678:9abc:4321::/64"), 2200);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0x12, 2210);
    ASSERT(ip1.getByte(1) == 0x34, 2220);
    ASSERT(ip1.getByte(2) == 0x56, 2230);
    ASSERT(ip1.getByte(3) == 0x78, 2240);
    ASSERT(ip1.getByte(4) == 0x9a, 2250);
    ASSERT(ip1.getByte(5) == 0xbc, 2260);
    ASSERT(ip1.getByte(6) == 0x43, 2270);
    ASSERT(ip1.getByte(7) == 0x21, 2280);
    ASSERT(ip1.getByte(8) == 0, 2290);
    ASSERT(ip1.getByte(9) == 0, 2300);
    ASSERT(ip1.getByte(10) == 0, 2310);
    ASSERT(ip1.getByte(11) == 0, 2320);
    ASSERT(ip1.getByte(12) == 0, 2330);
    ASSERT(ip1.getByte(13) == 0, 2340);
    ASSERT(ip1.getByte(14) == 0, 2350);
    ASSERT(ip1.getByte(15) == 0, 2360);
    ASSERT(ip1.getNetmask() == 64, 2370);


    ASSERT(ipp1.parse("1234:5678:9abc:4321:C000::/72"), 2400);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0x12, 2410);
    ASSERT(ip1.getByte(1) == 0x34, 2420);
    ASSERT(ip1.getByte(2) == 0x56, 2430);
    ASSERT(ip1.getByte(3) == 0x78, 2440);
    ASSERT(ip1.getByte(4) == 0x9a, 2450);
    ASSERT(ip1.getByte(5) == 0xbc, 2460);
    ASSERT(ip1.getByte(6) == 0x43, 2470);
    ASSERT(ip1.getByte(7) == 0x21, 2480);
    ASSERT(ip1.getByte(8) == 0xc0, 2490);
    ASSERT(ip1.getByte(9) == 0, 2500);
    ASSERT(ip1.getByte(10) == 0, 2510);
    ASSERT(ip1.getByte(11) == 0, 2520);
    ASSERT(ip1.getByte(12) == 0, 2530);
    ASSERT(ip1.getByte(13) == 0, 2540);
    ASSERT(ip1.getByte(14) == 0, 2550);
    ASSERT(ip1.getByte(15) == 0, 2560);
    ASSERT(ip1.getNetmask() == 72, 2570);


    // Test all zeroes

    ASSERT(ipp1.parse("::"), 2600);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0, 2610);
    ASSERT(ip1.getByte(1) == 0, 2620);
    ASSERT(ip1.getByte(2) == 0, 2630);
    ASSERT(ip1.getByte(3) == 0, 2640);
    ASSERT(ip1.getByte(4) == 0, 2650);
    ASSERT(ip1.getByte(5) == 0, 2660);
    ASSERT(ip1.getByte(6) == 0, 2670);
    ASSERT(ip1.getByte(7) == 0, 2680);
    ASSERT(ip1.getByte(8) == 0, 2690);
    ASSERT(ip1.getByte(9) == 0, 2700);
    ASSERT(ip1.getByte(10) == 0, 2710);
    ASSERT(ip1.getByte(11) == 0, 2720);
    ASSERT(ip1.getByte(12) == 0, 2730);
    ASSERT(ip1.getByte(13) == 0, 2740);
    ASSERT(ip1.getByte(14) == 0, 2750);
    ASSERT(ip1.getByte(15) == 0, 2760);
    ASSERT(ip1.getNetmask() == 128, 2770);


    // Middle zeroes
    ASSERT(ipp1.parse("fe80::222:19ff:fe5d:a133"), 2800);
    ip1 = ipp1.getIPv6();

    ASSERT(ip1.getByte(0) == 0xfe, 2810);
    ASSERT(ip1.getByte(1) == 0x80, 2820);
    ASSERT(ip1.getByte(2) == 0, 2830);
    ASSERT(ip1.getByte(3) == 0, 2840);
    ASSERT(ip1.getByte(4) == 0, 2850);
    ASSERT(ip1.getByte(5) == 0, 2860);
    ASSERT(ip1.getByte(6) == 0, 2870);
    ASSERT(ip1.getByte(7) == 0, 2880);
    ASSERT(ip1.getByte(8) == 0x02, 2890);
    ASSERT(ip1.getByte(9) == 0x22, 2900);
    ASSERT(ip1.getByte(10) == 0x19, 2910);
    ASSERT(ip1.getByte(11) == 0xff, 2920);
    ASSERT(ip1.getByte(12) == 0xfe, 2930);
    ASSERT(ip1.getByte(13) == 0x5d, 2940);
    ASSERT(ip1.getByte(14) == 0xa1, 2950);
    ASSERT(ip1.getByte(15) == 0x33, 2960);
    ASSERT(ip1.getNetmask() == 128, 2970);


    // Other invalid forms
    ASSERT(!ipp1.parse(""), 2800);
    ASSERT(!ipp1.parse(" "), 2810);


    printf("Tests Complete\n");
    return 0;
}

