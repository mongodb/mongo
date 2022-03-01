#!/usr/bin/perl
use LWP::Simple;
use IO::Handle;
$stdout = *STDOUT;
open(RES , ">resultlog.txt") || die "cannot open result log file";
#system("rm alldiff.txt in*.txt out*.txt");
for($i=10;$i<909;$i++)
{
   RES->printf("Test Page %d \n", $i);
   $url = "http://people.netscape.com/ftang/testscript/gb18030/gbtext.cgi?page=" . $i;
   RES->printf( "URL = %s\n", $url);
   $tmpfile = "> in". $i . ".txt";
   open STDOUT, $tmpfile || RES->print("cannot open " . $tmpfile . "\n");
   getprint $url;
   $cmd2 = "../../../dist/win32_d.obj/bin/nsconv -f GB18030 -t GB18030 in" . $i . ".txt out" . $i . ".txt >err";
   $cmd3 = "diff -u in" . $i . ".txt out" . $i . ".txt >> alldiff.txt";
   RES->printf( "Run  '%s'\n", $cmd2);
   $st2 = system($cmd2);
   RES->printf( "result = '%d'\n", $st2);
   RES->printf( "Run  '%s'\n", $cmd3);
   $st3 = system($cmd3);
   RES->printf( "result = '%d'\n", $st3);
}
