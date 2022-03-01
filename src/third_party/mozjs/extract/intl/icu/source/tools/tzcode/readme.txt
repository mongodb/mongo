* Copyright (C) 2016 and later: Unicode, Inc. and others.
* License & terms of use: http://www.unicode.org/copyright.html
**********************************************************************
* Copyright (c) 2003-2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: August 18 2003
* Since: ICU 2.8
**********************************************************************

Note:  this directory currently contains tzcode as of tzcode2014b.tar.gz
   with localtime.c  patches from tzcode2014b.tar.gz


----------------------------------------------------------------------
OVERVIEW

This file describes the tools in icu/source/tools/tzcode

The purpose of these tools is to process the zoneinfo or "Olson" time
zone database into a form usable by ICU4C (release 2.8 and later).
Unlike earlier releases, ICU4C 2.8 supports historical time zone
behavior, as well as the full set of Olson compatibility IDs.

References:

ICU4C:  http://www.icu-project.org/
Olson:  ftp://ftp.iana.org/tz/releases/

----------------------------------------------------------------------
ICU4C vs. ICU4J

For ICU releases >= 2.8, both ICU4C and ICU4J implement full
historical time zones, based on Olson data.  The implementations in C
and Java are somewhat different.  The C implementation is a
self-contained implementation, whereas ICU4J uses the underlying JDK
1.3 or 1.4 time zone implementation.

Older versions of ICU (C and Java <= 2.6) implement a "present day
snapshot".  This only reflects current time zone behavior, without
historical variation.  Furthermore, it lacks the full set of Olson
compatibility IDs.

----------------------------------------------------------------------
BACKGROUND

The zoneinfo or "Olson" time zone package is used by various systems
to describe the behavior of time zones.  The package consists of
several parts.  E.g.:

  Index of ftp://ftp.iana.org/tz/releases/

  tzcode2014b.tar.gz      172 KB       3/25/2014     05:11:00 AM
  tzdata2014b.tar.gz      216 KB       3/25/2014     05:11:00 AM

ICU only uses the tzdataYYYYV.tar.gz files,
where YYYY is the year and V is the version letter ('a'...'z').

This directory has partial contents of tzcode checked into ICU

----------------------------------------------------------------------
HOWTO

0. Note, these instructions will only work on POSIX type systems.

1. Obtain the current versions of tzdataYYYYV.tar.gz (aka `tzdata') from
   the FTP site given above.  Either manually download or use wget:

   $ cd {path_to}/icu/source/tools/tzcode
   $ wget "ftp://ftp.iana.org/tz/releases/tzdata*.tar.gz"

2. Copy only one tzdata*.tar.gz file into the icu/source/tools/tzcode/
   directory (this directory).

   *** Make sure you only have ONE FILE named tzdata*.tar.gz in the
       directory.

3. Build ICU normally. You will see a notice "updating zoneinfo.txt..."

### Following instructions for ICU maintainers only ###

4. Obtain the current version of tzcodeYYYY.tar.gz from the FTP site to
   this directory.

5. Run make target "check-dump".  This target extract makes the original
   tzcode and compile the original tzdata with icu supplemental data
   (icuzones).  Then it makes zdump / icuzdump and dump all time
   transitions for all ICU timezone to files under zdumpout / icuzdumpout
   directory.  When they produce different results, the target returns
   the error.

6. Don't forget to check in the new zoneinfo64.txt (from its location at
   {path_to}/icu/source/data/misc/zoneinfo64.txt) into SVN.

