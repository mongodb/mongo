#!/usr/bin/env perl
use v5.10;
use strict;
use warnings;
use utf8;
use open qw/:std :utf8/;
use Getopt::Long qw/GetOptions/;
use List::Util qw/first/;

my $update;
GetOptions ("update|u" => \$update) or die("Error in command line arguments\n");

my @whitelist;

my ($whitelist_file) = shift @ARGV;
if ( length $whitelist_file && -r $whitelist_file ) {
    open my $fh, "<", $whitelist_file or die "${whitelist_file}: $!";
    @whitelist = map {
        my ($file,$msg) = m{^([^:]+):\d+:\d+: (.*)};
        qr/\Q$file\E:\d+:\d+: \Q$msg\E/;
    } <$fh>;
}

my $out_fh = \*STDOUT;
if ( $update ) {
    open $out_fh, ">>", $whitelist_file;
}

my $exit_code = 0;
while (my $line = <STDIN>) {
    next if first { $line =~ $_ } @whitelist;
    $exit_code = 1;
    print {$out_fh} $line;
}

exit ($update ? 0 : $exit_code);
