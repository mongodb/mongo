/* libunwind - a platform-independent unwind library
   Copyright (C) 2003 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Copyright (c) 2003 Hewlett-Packard Co.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include <stdio.h>
#include <string.h>

#define UNW_LOCAL_ONLY	/* must define this for consistency with backtrace() */
#include <libunwind.h>

int verbose;

int
f257 (void)
{
  void *buffer[300];
  int i, n;

  if (verbose)
    printf ("First backtrace:\n");
  n = unw_backtrace (buffer, 300);
  if (verbose)
    for (i = 0; i < n; ++i)
      printf ("[%d] ip=%p\n", i, buffer[i]);

  unw_set_cache_size (unw_local_addr_space, 1023, 0);
  unw_flush_cache (unw_local_addr_space, 0, 0);

  if (verbose)
    printf ("\nSecond backtrace:\n");
  n = unw_backtrace (buffer, 300);
  if (verbose)
    for (i = 0; i < n; ++i)
      printf ("[%d] ip=%p\n", i, buffer[i]);
  return 0;
}

#define F(n,m)					\
int						\
f##n (void)					\
{						\
  return f##m ();				\
}

/* Here, we rely on the fact that the script-cache's hash-table is 256
   entries big.  With 257 functions, we're guaranteed to get at least
   one hash-collision.  */
F(256,257)	F(255,256)	F(254,255)	F(253,254)
F(252,253)	F(251,252)	F(250,251)	F(249,250)
F(248,249)	F(247,248)	F(246,247)	F(245,246)
F(244,245)	F(243,244)	F(242,243)	F(241,242)
F(240,241)	F(239,240)	F(238,239)	F(237,238)
F(236,237)	F(235,236)	F(234,235)	F(233,234)
F(232,233)	F(231,232)	F(230,231)	F(229,230)
F(228,229)	F(227,228)	F(226,227)	F(225,226)
F(224,225)	F(223,224)	F(222,223)	F(221,222)
F(220,221)	F(219,220)	F(218,219)	F(217,218)
F(216,217)	F(215,216)	F(214,215)	F(213,214)
F(212,213)	F(211,212)	F(210,211)	F(209,210)
F(208,209)	F(207,208)	F(206,207)	F(205,206)
F(204,205)	F(203,204)	F(202,203)	F(201,202)
F(200,201)	F(199,200)	F(198,199)	F(197,198)
F(196,197)	F(195,196)	F(194,195)	F(193,194)
F(192,193)	F(191,192)	F(190,191)	F(189,190)
F(188,189)	F(187,188)	F(186,187)	F(185,186)
F(184,185)	F(183,184)	F(182,183)	F(181,182)
F(180,181)	F(179,180)	F(178,179)	F(177,178)
F(176,177)	F(175,176)	F(174,175)	F(173,174)
F(172,173)	F(171,172)	F(170,171)	F(169,170)
F(168,169)	F(167,168)	F(166,167)	F(165,166)
F(164,165)	F(163,164)	F(162,163)	F(161,162)
F(160,161)	F(159,160)	F(158,159)	F(157,158)
F(156,157)	F(155,156)	F(154,155)	F(153,154)
F(152,153)	F(151,152)	F(150,151)	F(149,150)
F(148,149)	F(147,148)	F(146,147)	F(145,146)
F(144,145)	F(143,144)	F(142,143)	F(141,142)
F(140,141)	F(139,140)	F(138,139)	F(137,138)
F(136,137)	F(135,136)	F(134,135)	F(133,134)
F(132,133)	F(131,132)	F(130,131)	F(129,130)
F(128,129)	F(127,128)	F(126,127)	F(125,126)
F(124,125)	F(123,124)	F(122,123)	F(121,122)
F(120,121)	F(119,120)	F(118,119)	F(117,118)
F(116,117)	F(115,116)	F(114,115)	F(113,114)
F(112,113)	F(111,112)	F(110,111)	F(109,110)
F(108,109)	F(107,108)	F(106,107)	F(105,106)
F(104,105)	F(103,104)	F(102,103)	F(101,102)
F(100,101)	F(99,100)	F(98,99)	F(97,98)
F(96,97)	F(95,96)	F(94,95)	F(93,94)
F(92,93)	F(91,92)	F(90,91)	F(89,90)
F(88,89)	F(87,88)	F(86,87)	F(85,86)
F(84,85)	F(83,84)	F(82,83)	F(81,82)
F(80,81)	F(79,80)	F(78,79)	F(77,78)
F(76,77)	F(75,76)	F(74,75)	F(73,74)
F(72,73)	F(71,72)	F(70,71)	F(69,70)
F(68,69)	F(67,68)	F(66,67)	F(65,66)
F(64,65)	F(63,64)	F(62,63)	F(61,62)
F(60,61)	F(59,60)	F(58,59)	F(57,58)
F(56,57)	F(55,56)	F(54,55)	F(53,54)
F(52,53)	F(51,52)	F(50,51)	F(49,50)
F(48,49)	F(47,48)	F(46,47)	F(45,46)
F(44,45)	F(43,44)	F(42,43)	F(41,42)
F(40,41)	F(39,40)	F(38,39)	F(37,38)
F(36,37)	F(35,36)	F(34,35)	F(33,34)
F(32,33)	F(31,32)	F(30,31)	F(29,30)
F(28,29)	F(27,28)	F(26,27)	F(25,26)
F(24,25)	F(23,24)	F(22,23)	F(21,22)
F(20,21)	F(19,20)	F(18,19)	F(17,18)
F(16,17)	F(15,16)	F(14,15)	F(13,14)
F(12,13)	F(11,12)	F(10,11)	F(9,10)
F(8,9)		F(7,8)		F(6,7)		F(5,6)
F(4,5)		F(3,4)		F(2,3)		F(1,2)

int
main (int argc, char **argv)
{
  if (argc > 1 && strcmp (argv[1], "-v") == 0)
    verbose = 1;

  return f1 ();
}
