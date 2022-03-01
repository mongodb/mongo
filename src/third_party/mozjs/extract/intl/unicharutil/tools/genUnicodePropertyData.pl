#!/usr/bin/env perl

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This tool is used to prepare lookup tables of Unicode character properties
# needed by gfx code to support text shaping operations. The properties are
# read from the Unicode Character Database and compiled into multi-level arrays
# for efficient lookup.
#
# Note that for most properties, we now rely on ICU; this tool and the tables
# it generates are used only for a couple of properties not readily exposed
# via ICU APIs.
#
# To regenerate the tables in nsUnicodePropertyData.cpp:
#
# (1) Download the current Unicode data files from
#
#         https://www.unicode.org/Public/UNIDATA/
#
#     NB: not all the files are actually needed; currently, we require
#       - UnicodeData.txt
#       - ReadMe.txt (to record version/date of the UCD)
#       - Unihan_Variants.txt (from Unihan.zip)
#     though this may change if we find a need for additional properties.
#
#     The Unicode data files listed above should be together in one directory.
#
#     We also require the file
#        https://www.unicode.org/Public/security/latest/IdentifierStatus.txt
#     This file should be in a sub-directory "security" immediately below the
#        directory containing the other Unicode data files.
#
#     We also require the latest data file for UTR50, currently revision-17:
#        https://www.unicode.org/Public/vertical/revision-17/VerticalOrientation-17.txt
#     This file should be in a sub-directory "vertical" immediately below the
#        directory containing the other Unicode data files.
#
#
# (2) Run this tool using a command line of the form
#
#         perl genUnicodePropertyData.pl      \
#                 /path/to/icu/common/unicode \
#                 /path/to/UCD-directory
#
#     This will generate (or overwrite!) the files
#
#         nsUnicodePropertyData.cpp
#         nsUnicodeScriptCodes.h
#
#     in the current directory.

use strict;
use List::Util qw(first);

if ($#ARGV != 1) {
    print <<__EOT;
# Run this tool using a command line of the form
#
#     perl genUnicodePropertyData.pl      \\
#             /path/to/icu/common/unicode \\
#             /path/to/UCD-directory
#
# where icu/common/unicode is the directory containing ICU 'common' headers,
# and UCD-directory is a directory containing the current Unicode Character
# Database files (UnicodeData.txt, etc), available from
# https://www.unicode.org/Public/UNIDATA/, with additional resources as
# detailed in the source comments.
#
# This will generate (or overwrite!) the files
#
#     nsUnicodePropertyData.cpp
#     nsUnicodeScriptCodes.h
#
# in the current directory.
__EOT
    exit 0;
}

my $ICU = $ARGV[0];
my $UNICODE = $ARGV[1];

my @scriptCodeToName;
my @idtype;

my $sc = -1;

sub readIcuHeader
{
    my $file = shift;
    open FH, "< $ICU/$file" or die "can't open ICU header $ICU/$file\n";
    while (<FH>) {
        # adjust for ICU vs UCD naming discrepancies
        s/LANNA/TAI_THAM/;
        s/MEITEI_MAYEK/MEETEI_MAYEK/;
        s/ORKHON/OLD_TURKIC/;
        s/MENDE/MENDE_KIKAKUI/;
        s/SIGN_WRITING/SIGNWRITING/;
        if (m|USCRIPT_([A-Z_]+)\s*=\s*([0-9]+),\s*/\*\s*([A-Z][a-z]{3})\s*\*/|) {
            $sc = $2;
            $scriptCodeToName[$sc] = $1;
        }
    }
    close FH;
}

&readIcuHeader("uscript.h");

die "didn't find ICU script codes\n" if $sc == -1;

# We don't currently store these values; %idType is used only to check that
# properties listed in the IdentifierType.txt file are recognized. We record
# only the %mappedIdType values that are used by nsIDNService::isLabelSafe.
# In practice, it would be sufficient for us to read only the last value in
# IdentifierType.txt, but we check that all values are known so that we'll get
# a warning if future updates introduce new ones, and can consider whether
# they need to be taken into account.
my %idType = (
  "Not_Character"     => 0,
  "Recommended"       => 1,
  "Inclusion"         => 2,
  "Uncommon_Use"      => 3,
  "Technical"         => 4,
  "Obsolete"          => 5,
  "Aspirational"      => 6,
  "Limited_Use"       => 7,
  "Exclusion"         => 8,
  "Not_XID"           => 9,
  "Not_NFKC"          => 10,
  "Default_Ignorable" => 11,
  "Deprecated"        => 12
);

# These match the IdentifierType enum in nsUnicodeProperties.h.
my %mappedIdType = (
  "Restricted"   => 0,
  "Allowed"      => 1
);

my %verticalOrientationCode = (
  'U' => 0,  #   U - Upright, the same orientation as in the code charts
  'R' => 1,  #   R - Rotated 90 degrees clockwise compared to the code charts
  'Tu' => 2, #   Tu - Transformed typographically, with fallback to Upright
  'Tr' => 3  #   Tr - Transformed typographically, with fallback to Rotated
);

# initialize default properties
my @hanVariant;
my @fullWidth;
my @fullWidthInverse;
my @verticalOrientation;
for (my $i = 0; $i < 0x110000; ++$i) {
    $hanVariant[$i] = 0;
    $fullWidth[$i] = 0;
    $fullWidthInverse[$i] = 0;
    $verticalOrientation[$i] = 1; # default for unlisted codepoints is 'R'
}

# read ReadMe.txt
my @versionInfo;
open FH, "< $UNICODE/ReadMe.txt" or die "can't open Unicode ReadMe.txt file\n";
while (<FH>) {
    chomp;
    push @versionInfo, $_;
}
close FH;

# read UnicodeData.txt
open FH, "< $UNICODE/UnicodeData.txt" or die "can't open UCD file UnicodeData.txt\n";
while (<FH>) {
    chomp;
    my @fields = split /;/;
    if ($fields[1] =~ /First/) {
        my $first = hex "0x$fields[0]";
        $_ = <FH>;
        @fields = split /;/;
        if ($fields[1] =~ /Last/) {
            my $last = hex "0x$fields[0]";
            do {
                if ($fields[1] =~ /CJK/) {
                  @hanVariant[$first] = 3;
                }
                $first++;
            } while ($first <= $last);
        } else {
            die "didn't find Last code for range!\n";
        }
    } else {
        my $usv = hex "0x$fields[0]";
        if ($fields[1] =~ /CJK/) {
          @hanVariant[$usv] = 3;
        }
        if ($fields[5] =~ /^<narrow>/) {
          my $wideChar = hex(substr($fields[5], 9));
          die "didn't expect supplementary-plane values here" if $usv > 0xffff || $wideChar > 0xffff;
          $fullWidth[$usv] = $wideChar;
          $fullWidthInverse[$wideChar] = $usv;
        }
        elsif ($fields[5] =~ /^<wide>/) {
          my $narrowChar = hex(substr($fields[5], 7));
          die "didn't expect supplementary-plane values here" if $usv > 0xffff || $narrowChar > 0xffff;
          $fullWidth[$narrowChar] = $usv;
          $fullWidthInverse[$usv] = $narrowChar;
        }
    }
}
close FH;

# read IdentifierStatus.txt
open FH, "< $UNICODE/security/IdentifierStatus.txt" or die "can't open UCD file IdentifierStatus.txt\n";
push @versionInfo, "";
while (<FH>) {
  chomp;
  s/\xef\xbb\xbf//;
  push @versionInfo, $_;
  last if /Date:/;
}
while (<FH>) {
  if (m/([0-9A-F]{4,6})(?:\.\.([0-9A-F]{4,6}))*\s+;\s+Allowed/) {
    my $start = hex "0x$1";
    my $end = (defined $2) ? hex "0x$2" : $start;
    for (my $i = $start; $i <= $end; ++$i) {
      $idtype[$i] = $mappedIdType{'Allowed'};
    }
  }
}
close FH;

open FH, "< $UNICODE/Unihan_Variants.txt" or die "can't open UCD file Unihan_Variants.txt (from Unihan.zip)\n";
push @versionInfo, "";
while (<FH>) {
  chomp;
  push @versionInfo, $_;
  last if /Date:/;
}
my $savedusv = 0;
my $hasTC = 0;
my $hasSC = 0;
while (<FH>) {
  chomp;
  if (m/U\+([0-9A-F]{4,6})\s+k([^ ]+)Variant/) {
    my $usv = hex "0x$1";
    if ($usv != $savedusv) {
      unless ($savedusv == 0) {
        if ($hasTC && !$hasSC) {
          $hanVariant[$savedusv] = 1;
        } elsif (!$hasTC && $hasSC) {
          $hanVariant[$savedusv] = 2;
        }
      }
      $savedusv = $usv;
      $hasTC = 0;
      $hasSC = 0;
    }
    if ($2 eq "Traditional") {
      $hasTC = 1;
    }
    if ($2 eq "Simplified") {
      $hasSC = 1;
    }
  } 
}
close FH;

# read VerticalOrientation-17.txt
open FH, "< $UNICODE/vertical/VerticalOrientation-17.txt" or die "can't open UTR50 data file VerticalOrientation-17.txt\n";
push @versionInfo, "";
while (<FH>) {
    chomp;
    push @versionInfo, $_;
    last if /Date:/;
}
while (<FH>) {
    chomp;
    s/#.*//;
    if (m/([0-9A-F]{4,6})(?:\.\.([0-9A-F]{4,6}))*\s*;\s*([^ ]+)/) {
        my $vo = $3;
        warn "unknown Vertical_Orientation code $vo"
            unless exists $verticalOrientationCode{$vo};
        $vo = $verticalOrientationCode{$vo};
        my $start = hex "0x$1";
        my $end = (defined $2) ? hex "0x$2" : $start;
        for (my $i = $start; $i <= $end; ++$i) {
            $verticalOrientation[$i] = $vo;
        }
    }
}
close FH;

my $timestamp = gmtime();

open DATA_TABLES, "> nsUnicodePropertyData.cpp" or die "unable to open nsUnicodePropertyData.cpp for output";

my $licenseBlock = q[
/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Derived from the Unicode Character Database by genUnicodePropertyData.pl
 *
 * For Unicode terms of use, see http://www.unicode.org/terms_of_use.html
 */
];

my $versionInfo = join("\n", @versionInfo);

print DATA_TABLES <<__END;
$licenseBlock
/*
 * Created on $timestamp from UCD data files with version info:
 *

$versionInfo

 *
 * * * * * This file contains MACHINE-GENERATED DATA, do not edit! * * * * *
 */

#include <stdint.h>
#include "harfbuzz/hb.h"

__END

open HEADER, "> nsUnicodeScriptCodes.h" or die "unable to open nsUnicodeScriptCodes.h for output";

print HEADER <<__END;
$licenseBlock
/*
 * Created on $timestamp from UCD data files with version info:
 *

$versionInfo

 *
 * * * * * This file contains MACHINE-GENERATED DATA, do not edit! * * * * *
 */

#ifndef NS_UNICODE_SCRIPT_CODES
#define NS_UNICODE_SCRIPT_CODES

__END

our $totalData = 0;

sub sprintCharProps2_short
{
  my $usv = shift;
  return sprintf("{%d,%d},",
                 $verticalOrientation[$usv], $idtype[$usv]);
}
my $type = q|
struct nsCharProps2 {
  // Currently only 4 bits are defined here, so 4 more could be added without
  // affecting the storage requirements for this struct. Or we could pack two
  // records per byte, at the cost of a slightly more complex accessor.
  unsigned char mVertOrient:2;
  unsigned char mIdType:2;
};
|;
&genTables("CharProp2", $type, "nsCharProps2", 9, 7, \&sprintCharProps2_short, 16, 1, 1);

sub sprintHanVariants
{
  my $baseUsv = shift;
  my $varShift = 0;
  my $val = 0;
  while ($varShift < 8) {
    $val |= $hanVariant[$baseUsv++] << $varShift;
    $varShift += 2;
  }
  return sprintf("0x%02x,", $val);
}
## Han Variant data currently unused but may be needed in future, see bug 857481
## &genTables("HanVariant", "", "uint8_t", 9, 7, \&sprintHanVariants, 2, 1, 4);

sub sprintFullWidth
{
  my $usv = shift;
  return sprintf("0x%04x,", $fullWidth[$usv]);
}
&genTables("FullWidth", "", "uint16_t", 10, 6, \&sprintFullWidth, 0, 2, 1);

sub sprintFullWidthInverse
{
  my $usv = shift;
  return sprintf("0x%04x,", $fullWidthInverse[$usv]);
}
&genTables("FullWidthInverse", "", "uint16_t", 10, 6, \&sprintFullWidthInverse, 0, 2, 1);

print STDERR "Total data = $totalData\n";

sub genTables
{
  my ($prefix, $typedef, $type, $indexBits, $charBits, $func, $maxPlane, $bytesPerEntry, $charsPerEntry) = @_;

  if ($typedef ne '') {
    print HEADER "$typedef\n";
  }

  print DATA_TABLES "#define k${prefix}MaxPlane  $maxPlane\n";
  print DATA_TABLES "#define k${prefix}IndexBits $indexBits\n";
  print DATA_TABLES "#define k${prefix}CharBits  $charBits\n";

  my $indexLen = 1 << $indexBits;
  my $charsPerPage = 1 << $charBits;
  my %charIndex = ();
  my %pageMapIndex = ();
  my @pageMap = ();
  my @char = ();
  
  my $planeMap = "\x00" x $maxPlane;
  foreach my $plane (0 .. $maxPlane) {
    my $pageMap = "\x00" x $indexLen * 2;
    foreach my $page (0 .. $indexLen - 1) {
        my $charValues = "";
        for (my $ch = 0; $ch < $charsPerPage; $ch += $charsPerEntry) {
            my $usv = $plane * 0x10000 + $page * $charsPerPage + $ch;
            $charValues .= &$func($usv);
        }
        chop $charValues;

        unless (exists $charIndex{$charValues}) {
            $charIndex{$charValues} = scalar keys %charIndex;
            $char[$charIndex{$charValues}] = $charValues;
        }
        substr($pageMap, $page * 2, 2) = pack('S', $charIndex{$charValues});
    }
    
    unless (exists $pageMapIndex{$pageMap}) {
        $pageMapIndex{$pageMap} = scalar keys %pageMapIndex;
        $pageMap[$pageMapIndex{$pageMap}] = $pageMap;
    }
    if ($plane > 0) {
        substr($planeMap, $plane - 1, 1) = pack('C', $pageMapIndex{$pageMap});
    }
  }

  if ($maxPlane) {
    print DATA_TABLES "static const uint8_t s${prefix}Planes[$maxPlane] = {";
    print DATA_TABLES join(',', map { sprintf("%d", $_) } unpack('C*', $planeMap));
    print DATA_TABLES "};\n\n";
  }

  my $chCount = scalar @char;
  my $pmBits = $chCount > 255 ? 16 : 8;
  my $pmCount = scalar @pageMap;
  if ($maxPlane == 0) {
    die "there should only be one pageMap entry!" if $pmCount > 1;
    print DATA_TABLES "static const uint${pmBits}_t s${prefix}Pages[$indexLen] = {\n";
  } else {
    print DATA_TABLES "static const uint${pmBits}_t s${prefix}Pages[$pmCount][$indexLen] = {\n";
  }
  for (my $i = 0; $i < scalar @pageMap; ++$i) {
    print DATA_TABLES $maxPlane > 0 ? "  {" : "  ";
    print DATA_TABLES join(',', map { sprintf("%d", $_) } unpack('S*', $pageMap[$i]));
    print DATA_TABLES $maxPlane > 0 ? ($i < $#pageMap ? "},\n" : "}\n") : "\n";
  }
  print DATA_TABLES "};\n\n";

  my $pageLen = $charsPerPage / $charsPerEntry;
  print DATA_TABLES "static const $type s${prefix}Values[$chCount][$pageLen] = {\n";
  for (my $i = 0; $i < scalar @char; ++$i) {
    print DATA_TABLES "  {";
    print DATA_TABLES $char[$i];
    print DATA_TABLES $i < $#char ? "},\n" : "}\n";
  }
  print DATA_TABLES "};\n";

  my $dataSize = $pmCount * $indexLen * $pmBits/8 +
                 $chCount * $pageLen * $bytesPerEntry + 
                 $maxPlane;
  $totalData += $dataSize;

  print STDERR "Data for $prefix = $dataSize\n";
}

print DATA_TABLES <<__END;
/*
 * * * * * This file contains MACHINE-GENERATED DATA, do not edit! * * * * *
 */
__END

close DATA_TABLES;

print HEADER "namespace mozilla {\n";
print HEADER "namespace unicode {\n";
print HEADER "enum class Script : int16_t {\n";
for (my $i = 0; $i < scalar @scriptCodeToName; ++$i) {
  print HEADER "  ", $scriptCodeToName[$i], " = ", $i, ",\n";
}
print HEADER "\n  NUM_SCRIPT_CODES = ", scalar @scriptCodeToName, ",\n";
print HEADER "\n  INVALID = -1\n";
print HEADER "};\n";
print HEADER "} // namespace unicode\n";
print HEADER "} // namespace mozilla\n\n";

print HEADER <<__END;
#endif
/*
 * * * * * This file contains MACHINE-GENERATED DATA, do not edit! * * * * *
 */
__END

close HEADER;

