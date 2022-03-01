#!/usr/bin/env perl

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

# This tool is used to extract "special" (one-to-many) case mappings
# into a form that can be used by nsTextRunTransformations.

use strict;

if ($#ARGV != 1) {
  print <<__EOT;
# Run this tool using a command line of the form
#
#     perl genSpecialCasingData.pl UnicodeData.txt SpecialCasing.txt
#
# The nsSpecialCasingData.cpp file will be written to standard output.
#
# This tool will also write up-to-date versions of the test files
#     all-{upper,lower,title}.html
# and corresponding -ref files in the current directory.
#
__EOT
  exit 0;
}

my %allLower;
my %allUpper;
my %allTitle;
my %compositions;
my %gc;
open FH, "< $ARGV[0]" or die "can't open $ARGV[0] (should be UnicodeData.txt)\n";
while (<FH>) {
  chomp;
  my @fields = split /;/;
  next if ($fields[1] =~ /</); # ignore ranges etc
  my $usv = hex "0x$fields[0]";
  $allUpper{$usv} = $fields[12] if $fields[12] ne '';
  $allLower{$usv} = $fields[13] if $fields[13] ne '';
  $allTitle{$usv} = $fields[14] if $fields[14] ne '';
  $gc{$usv} = $fields[2];
  # we only care about non-singleton canonical decomps
  my $decomp = $fields[5];
  next if $decomp eq '' or $decomp =~ /</ or not $decomp =~ / /;
  $compositions{$decomp} = sprintf("%04X", $usv);
}
close FH;

my %specialLower;
my %specialUpper;
my %specialTitle;
my %charName;
my @headerLines;
open FH, "< $ARGV[1]" or die "can't open $ARGV[1] (should be SpecialCasing.txt)\n";
while (<FH>) {
  chomp;
  m/#\s*(.+)$/;
  my $comment = $1;
  if ($comment =~ /^(SpecialCasing-|Date:)/) {
    push @headerLines, $comment;
    next;
  }
  s/#.*//;
  s/;\s*$//;
  next if $_ eq '';
  my @fields = split /; */;
  next unless (scalar @fields) == 4;
  my $usv = hex "0x$fields[0]";
  addIfSpecial(\%specialLower, $usv, $fields[1]);
  addIfSpecial(\%specialTitle, $usv, $fields[2]);
  addIfSpecial(\%specialUpper, $usv, $fields[3]);
  $charName{$usv} = $comment;
}
close FH;

print <<__END__;
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Auto-generated from files in the Unicode Character Database
   by genSpecialCasingData.pl - do not edit! */

#include "nsSpecialCasingData.h"
#include "mozilla/ArrayUtils.h" // for ArrayLength
#include <stdlib.h>       // for bsearch

__END__
map { print "/* $_ */\n" } @headerLines;

print <<__END__;

using mozilla::unicode::MultiCharMapping;

__END__

printMappings('Lower', \%specialLower);
printMappings('Upper', \%specialUpper);
printMappings('Title', \%specialTitle);

print <<__END__;
static int CompareMCM(const void* aKey, const void* aElement)
{
  const uint32_t ch = *static_cast<const uint32_t*>(aKey);
  const MultiCharMapping* mcm = static_cast<const MultiCharMapping*>(aElement);
  return int(ch) - int(mcm->mOriginalChar);
}

#define MAKE_SPECIAL_CASE_ACCESSOR(which) \\
  const MultiCharMapping* \\
  Special##which(uint32_t aChar) \\
  { \\
    const void* p = bsearch(&aChar, CaseSpecials_##which, \\
                            mozilla::ArrayLength(CaseSpecials_##which), \\
                            sizeof(MultiCharMapping), CompareMCM); \\
    return static_cast<const MultiCharMapping*>(p); \\
  }

namespace mozilla {
namespace unicode {

MAKE_SPECIAL_CASE_ACCESSOR(Lower)
MAKE_SPECIAL_CASE_ACCESSOR(Upper)
MAKE_SPECIAL_CASE_ACCESSOR(Title)

} // namespace unicode
} // namespace mozilla
__END__

addSpecialsTo(\%allLower, \%specialLower);
addSpecialsTo(\%allUpper, \%specialUpper);
addSpecialsTo(\%allTitle, \%specialTitle);

my $testFont = "../fonts/dejavu-sans/DejaVuSans.ttf";
genTest('lower', \%allLower);
genTest('upper', \%allUpper);
genTitleTest();

sub printMappings {
  my ($whichMapping, $hash) = @_;
  print "static const MultiCharMapping CaseSpecials_${whichMapping}[] = {\n";
  foreach my $key (sort { $a <=> $b } keys %$hash) {
    my @chars = split(/ /, $hash->{$key});
    printf "  { 0x%04x, {0x%04x, 0x%04x, 0x%04x} }, // %s\n", $key,
           hex "0x0$chars[0]", hex "0x0$chars[1]", hex "0x0$chars[2]",
           "$charName{$key}";
  }
  print "};\n\n";
};

sub addIfSpecial {
  my ($hash, $usv, $mapping) = @_;
  return unless $mapping =~ / /;
  # only do compositions that start with the initial char
  foreach (keys %compositions) {
    $mapping =~ s/^$_/$compositions{$_}/;
  }
  $hash->{$usv} = $mapping;
};

sub addSpecialsTo {
  my ($hash, $specials) = @_;
  foreach my $key (keys %$specials) {
    $hash->{$key} = $specials->{$key};
  }
};

sub genTest {
  my ($whichMapping, $hash) = @_;
  open OUT, "> all-$whichMapping.html";
  print OUT <<__END__;
<!DOCTYPE html>
<!-- GENERATED FILE, DO NOT EDIT -->
<html>
 <head>
  <meta http-equiv="Content-type" content="text/html; charset=utf-8">
  <style type="text/css">
   \@font-face { font-family: foo; src: url($testFont); }
   p { font-family: foo; font-size: 12px; text-transform: ${whichMapping}case; }
  </style>
 </head>
 <body>
  <p>
__END__
  foreach my $key (sort { $a <=> $b } keys %$hash) {
    # Bug 1476304: we exclude Georgian letters U+10D0..10FF because of lack
    # of widespread font support for the corresponding Mtavruli characters
    # at this time (July 2018).
    # This condition is to be removed once the major platforms ship with
    # fonts that support U+1C90..1CBF.
    my $skippedGeorgian = $whichMapping eq "upper" && $key >= 0x10D0 && $key <= 0x10FF;
    print OUT "<!-- " if $skippedGeorgian;
    printf OUT "&#x%04X;", $key;
    print OUT " -->" if $skippedGeorgian;
    print OUT " <!-- $charName{$key} -->" if exists $charName{$key};
    print OUT " <!-- Temporarily skipped, see bug 1476304. -->" if $skippedGeorgian;
    print OUT "\n";
  }
  print OUT <<__END__;
  </p>
 </body>
</html>
__END__
  close OUT;

  open OUT, "> all-$whichMapping-ref.html";
  print OUT <<__END__;
<!DOCTYPE html>
<!-- GENERATED FILE, DO NOT EDIT -->
<html>
 <head>
  <meta http-equiv="Content-type" content="text/html; charset=utf-8">
  <style type="text/css">
   \@font-face { font-family: foo; src: url($testFont); }
   p { font-family: foo; font-size: 12px; }
  </style>
 </head>
 <body>
  <p>
__END__
  foreach my $key (sort { $a <=> $b } keys %$hash) {
    # Bug 1476304: we exclude Georgian letters U+10D0..10FF because of lack
    # of widespread font support for the corresponding Mtavruli characters
    # at this time (July 2018).
    # This condition is to be removed once the major platforms ship with
    # fonts that support U+1C90..1CBF.
    my $skippedGeorgian = $whichMapping eq "upper" && $key >= 0x10D0 && $key <= 0x10FF;
    print OUT "<!-- " if $skippedGeorgian;
    print OUT join('', map { sprintf("&#x%s;", $_) } split(/ /, $hash->{$key}));
    print OUT " -->" if $skippedGeorgian;
    print OUT " <!-- $charName{$key} -->" if exists $charName{$key};
    print OUT " <!-- Temporarily skipped, see bug 1476304. -->" if $skippedGeorgian;
    print OUT "\n";
  }
  print OUT <<__END__;
  </p>
 </body>
</html>
__END__
  close OUT;
};

sub genTitleTest {
  open OUT, "> all-title.html";
  print OUT <<__END__;
<!DOCTYPE html>
<!-- GENERATED FILE, DO NOT EDIT -->
<html>
 <head>
  <meta http-equiv="Content-type" content="text/html; charset=utf-8">
  <style type="text/css">
   \@font-face { font-family: foo; src: url($testFont); }
   p { font-family: foo; text-transform: capitalize; }
  </style>
 </head>
 <body>
  <p>
__END__
  foreach my $key (sort { $a <=> $b } keys %allTitle) {
    printf OUT "&#x%04X;x", $key;
    print OUT " <!-- $charName{$key} -->" if exists $charName{$key};
    print OUT "\n";
  }
  print OUT <<__END__;
  </p>
 </body>
</html>
__END__
  close OUT;

  open OUT, "> all-title-ref.html";
  print OUT <<__END__;
<!DOCTYPE html>
<!-- GENERATED FILE, DO NOT EDIT -->
<html>
 <head>
  <meta http-equiv="Content-type" content="text/html; charset=utf-8">
  <style type="text/css">
   \@font-face { font-family: foo; src: url($testFont); }
   p { font-family: foo; }
  </style>
 </head>
 <body>
  <p>
__END__
  foreach my $key (sort { $a <=> $b } keys %allTitle) {
    # capitalize is only applied to characters with GC=L* or N*...
    if ($gc{$key} =~ /^[LN]/) {
      # ...and those that are already uppercase are not transformed
      if (exists $allUpper{$key}) {
        print OUT join('', map { sprintf("&#x%s;", $_) } split(/ /, $allTitle{$key}));
      } else {
        printf OUT "&#x%04X;", $key;
      }
      print OUT "x";
    } else {
      printf OUT "&#x%04X;X", $key;
    }
    print OUT " <!-- $charName{$key} -->" if exists $charName{$key};
    print OUT "\n";
  }
  print OUT <<__END__;
  </p>
 </body>
</html>
__END__
  close OUT;
};
