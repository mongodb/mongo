#!/usr/bin/perl -w
#          Copyright Steven J. Ross 2008 - 2014
# Distributed under the Boost Software License, Version 1.0.
#    (See accompanying file LICENSE_1_0.txt or copy at
#          http://www.boost.org/LICENSE_1_0.txt)
#
# See http://www.boost.org/libs/sort for library home page.

# A speed and accuracy testing and automated parameter tuning script.

$usage = "usage: tune.pl [-tune] [-real] [-tune_verify] [-verbose] [-multiple_iterations] [-large] [-small] [-windows] [fileSize]\n";
# testing sorting on 40 million elements by default
# don't test on below 2^22 (4 million) elements as that is the minimum
# for max_splits of 11 to be efficient
use File::Compare;
$defFileSize = 5000000;
$loopCount = 1;
$realtimes = 0;
$verifycorrect = 0;
$verbose = 0;
$exename = "spreadsort";
$makename = "b2 \-\-tune";
$all = "";
$iter_count = 1;
$debug = 0;
$log = "> .tunelog";
$log2 = "> .tunelog 2>&1";
$diffopt = "-q";
$tune = 0;
# have to change the path for UNIX
$prev_path = $ENV{'PATH'}; 
$ENV{'PATH'} = '.:'.$prev_path;

for (my $ii = 0; $ii < @ARGV; $ii++) {
        my $currArg = $ARGV[$ii];
        if ($currArg =~ /^-help$/) {
            print STDERR $usage;
            exit(0);
        }
        # verification roughly doubles the runtime of this script,
        # but it does make sure that results are correct during tuning
        # verification always runs during speed comparisons with std::sort
        if ($currArg =~ /^-tune_verify$/) {
            $verifycorrect = 1;
        # use real times only, don't use weighting and special-case tests
        # this saves about 5/6 of the script runtime but results are different
        } elsif ($currArg =~ /^-real$/) {
            $realtimes = 1;
        } elsif ($currArg =~ /^-verbose$/) {
            $verbose = 1;
        # runs until we converge on a precise set of values
        # defaults off because of runtime
        } elsif ($currArg =~ /^-multiple_iterations$/) {
            $iter_count = 4;
        } elsif ($currArg =~ /^-debug$/) {
            $debug = 1;
            $log = "";
            $diffopt = "";
        } elsif ($currArg =~ /^-large$/) {
            $defFileSize = 20000000;
        } elsif ($currArg =~ /^-small$/) {
            $defFileSize = 100000;
        } elsif ($currArg =~ /^-tune$/) {
            $tune = 1;
        } elsif ($currArg =~ /^-windows$/) {
            $makename = "..\\..\\".$makename;
        } elsif ($currArg =~ /^-/) {
            print STDERR $usage;
            exit(0);
        } else {
                $defFileSize = $currArg;
        }
}
$fileSize = $defFileSize;

print STDOUT "Tuning variables for $exename on vectors with $defFileSize elements\n";

# these are reasonable values
$max_splits = 11;
$log_finishing_count = 31;
$log_min_size = 11;
$log_mean_bin_size = 2;
$float_log_min_size = 10;
$float_log_mean_bin_size = 2;
$float_log_finishing_count = 4;

# this value is a minimum to obtain decent file I/O performance
$min_sort_size = 1000;
$std = "";

print STDOUT "building randomgen\n";
system("$makename randomgen $log");
# Tuning to get convergence, maximum of 4 iterations with multiple iterations
# option set
$changed = 1;
my $ii = 0;
if ($tune) {
    for ($ii = 0; $changed and $ii < $iter_count; $ii++) {
        $changed = 0;
        # Tuning max_splits is not recommended.
        #print STDOUT "Tuning max_splits\n";
        #TuneVariable(\$max_splits, $log_min_size - $log_mean_bin_size, 17);
        print STDOUT "Tuning log of the minimum count for recursion\n";
        TuneVariable(\$log_min_size, $log_mean_bin_size + 1, $max_splits + $log_mean_bin_size);
        print STDOUT "Tuning log_mean_bin_size\n";
        TuneVariable(\$log_mean_bin_size, 0, $log_min_size - 1);
        print STDOUT "Tuning log_finishing_size\n";
        TuneVariable(\$log_finishing_count, 1, $log_min_size);
        # tuning variables for floats
        $exename = "floatsort";
        print STDOUT "Tuning log of the minimum count for recursion for floats\n";
        TuneVariable(\$float_log_min_size, $float_log_mean_bin_size + 1, $max_splits + $float_log_mean_bin_size);
        print STDOUT "Tuning float_log_mean_bin_size\n";
        TuneVariable(\$float_log_mean_bin_size, 0, $float_log_min_size - 1);
        print STDOUT "Tuning float_log_finishing_size\n";
        TuneVariable(\$float_log_finishing_count, 1, $float_log_min_size);
        $exename = "spreadsort";
    }

    # After optimizations for large datasets are complete, see how small of a 
    # dataset can be sped up
    print STDOUT "Tuning minimum sorting size\n";
    TuneMinSize();
    print STDOUT "Writing results\n";
}

# Doing a final run with final settings to compare sort times
# also verifying correctness of results
$verifycorrect = 1;
$loopCount = 1;
$fileSize = $defFileSize;
system("$makename $all $log");
$std = "";
PerfTest("Verifying integer_sort", "spreadsort");
PerfTest("Verifying float_sort", "floatsort");
PerfTest("Verifying string_sort", "stringsort");
PerfTest("Verifying integer_sort with mostly-sorted data", "mostlysorted");
PerfTest("Timing integer_sort on already-sorted data", "alreadysorted");
PerfTest("Verifying integer_sort with rightshift", "rightshift");
PerfTest("Verifying integer_sort with 64-bit integers", "int64");
PerfTest("Verifying integer_sort with separate key and data", "keyplusdata");
PerfTest("Verifying reverse integer_sort", "reverseintsort");
PerfTest("Verifying float_sort with doubles", "double");
PerfTest("Verifying float_sort with shift functor", "shiftfloatsort");
PerfTest("Verifying float_sort with functors", "floatfunctorsort");
PerfTest("Verifying string_sort with indexing functors", "charstringsort");
PerfTest("Verifying string_sort with all functors", "stringfunctorsort");
PerfTest("Verifying reverse_string_sort", "reversestringsort");
PerfTest("Verifying reverse_string_sort with functors",
         "reversestringfunctorsort");
PerfTest("Verifying generalized string_sort with multiple keys of different types",
         "generalizedstruct");
PerfTest("Verifying boost::sort on its custom-built worst-case distribution",
         "binaryalrbreaker");
# clean up once we finish
system("$makename clean $log");
# WINDOWS
system("del spread_sort_out.txt $log2");
system("del standard_sort_out.txt $log2");
system("del input.txt $log2");
system("del *.rsp $log2");
system("del *.manifest $log2");
system("del time.txt $log2");
# UNIX
system("rm -f time.txt $log2");
system("rm -f spread_sort_out.txt $log2");
system("rm -f standard_sort_out.txt $log2");
system("rm -f input.txt $log2");

$ENV{'PATH'} = $prev_path;

# A simple speed test comparing std::sort to 
sub PerfTest {
    my ($message, $local_exe) = @_;
    $exename = $local_exe;
    print STDOUT "$message\n";
    $lastTime = SumTimes();
    print STDOUT "runtime: $lastTime\n";
    print STDOUT "std::sort time: $baseTime\n";
    $speedup = (($baseTime/$lastTime) - 1) * 100;
    print STDOUT "speedup: ".sprintf("%.2f", $speedup)."%\n";
}

# Write an updated constants file as part of tuning.
sub WriteConstants {
    # deleting the file
    $const_file = 'include/boost/sort/spreadsort/detail/constants.hpp';
    @cannot = grep {not unlink} $const_file;
    print "$0: could not unlink @cannot\n" if @cannot;

    # writing the results back to the original file name
    unless(open(CONSTANTS, ">$const_file")) {
      print STDERR "Can't open output file: $const_file: $!\n";
      exit;
    }
    print CONSTANTS "//constant definitions for the Boost Sort library\n\n";
    print CONSTANTS "//          Copyright Steven J. Ross 2001 - 2014\n";
    print CONSTANTS "// Distributed under the Boost Software License, Version 1.0.\n";
    print CONSTANTS "//    (See accompanying file LICENSE_1_0.txt or copy at\n";
    print CONSTANTS "//          http://www.boost.org/LICENSE_1_0.txt)\n\n";
    print CONSTANTS "//  See http://www.boost.org/libs/sort for library home page.\n";
    print CONSTANTS "#ifndef BOOST_SORT_SPREADSORT_DETAIL_CONSTANTS\n";
    print CONSTANTS "#define BOOST_SORT_SPREADSORT_DETAIL_CONSTANTS\n";
    print CONSTANTS "namespace boost {\n";
    print CONSTANTS "namespace sort {\n";
    print CONSTANTS "namespace spreadsort {\n";
    print CONSTANTS "namespace detail {\n";
    print CONSTANTS "//Tuning constants\n";
    print CONSTANTS "//This should be tuned to your processor cache;\n"; 
    print CONSTANTS "//if you go too large you get cache misses on bins\n";
    print CONSTANTS "//The smaller this number, the less worst-case memory usage.\n";  
    print CONSTANTS "//If too small, too many recursions slow down spreadsort\n";
    print CONSTANTS "enum { max_splits = $max_splits,\n";
    print CONSTANTS "//It's better to have a few cache misses and finish sorting\n";
    print CONSTANTS "//than to run another iteration\n";
    print CONSTANTS "max_finishing_splits = max_splits + 1,\n";
    print CONSTANTS "//Sets the minimum number of items per bin.\n";
    print CONSTANTS "int_log_mean_bin_size = $log_mean_bin_size,\n";
    print CONSTANTS "//Used to force a comparison-based sorting for small bins, if it's faster.\n";
    print CONSTANTS "//Minimum value 1\n";
    $log_min_split_count = $log_min_size - $log_mean_bin_size;
    print CONSTANTS "int_log_min_split_count = $log_min_split_count,\n";
    print CONSTANTS "//This is the minimum split count to use spreadsort when it will finish in one\n";
    print CONSTANTS "//iteration.  Make this larger the faster std::sort is relative to integer_sort.\n";
    print CONSTANTS "int_log_finishing_count = $log_finishing_count,\n";
    print CONSTANTS "//Sets the minimum number of items per bin for floating point.\n";
    print CONSTANTS "float_log_mean_bin_size = $float_log_mean_bin_size,\n";
    print CONSTANTS "//Used to force a comparison-based sorting for small bins, if it's faster.\n";
    print CONSTANTS "//Minimum value 1\n";
    $float_log_min_split_count = $float_log_min_size - $float_log_mean_bin_size;
    print CONSTANTS "float_log_min_split_count = $float_log_min_split_count,\n";
    print CONSTANTS "//This is the minimum split count to use spreadsort when it will finish in one\n";
    print CONSTANTS "//iteration.  Make this larger the faster std::sort is relative to float_sort.\n";
    print CONSTANTS "float_log_finishing_count = $float_log_finishing_count,\n";
    print CONSTANTS "//There is a minimum size below which it is not worth using spreadsort\n";
    print CONSTANTS "min_sort_size = $min_sort_size };\n";
    print CONSTANTS "}\n}\n}\n}\n#endif\n";
    close CONSTANTS;
    system("$makename $exename $log");
}

# Sort the file with both std::sort and boost::sort, verify the results are the
# same, update stdtime with std::sort time, and return boost::sort time.
sub CheckTime {
    my $sort_time = 0.0;
    my $time_file = "time.txt";
    # use the line below on systems that can't overwrite.
    #system("rm -f $time_file");
    system("$exename $loopCount $std > $time_file");
    unless(open(CODE, $time_file)) {
        print STDERR "Could not open file: $time_file: $!\n";
        exit;
    }
    while ($line = <CODE>) {
        @parts = split("time", $line);
        if (@parts > 1) {
            $sort_time = $parts[1];
            last;
        }              
    }
    close(CODE);
    # verifying correctness
    if (not $std and $verifycorrect) {
        system("$exename $loopCount -std > $time_file");
        unless(open(CODE, $time_file)) {
            print STDERR "Could not open file: $time_file: $!\n";
            exit;
        }
        die "Difference in results\n" unless (compare("boost_sort_out.txt","standard_sort_out.txt") == 0) ;
        while ($line = <CODE>) {
            @parts = split("time", $line);
            if (@parts > 1) {
                $stdsingle = $parts[1];
                last;
            }               
        }
        close(CODE);
    }
    return $sort_time;
}

# Sum up times for different data distributions.  If realtimes is not set, 
# larger ranges are given a larger weight.
sub SumTimes {
    my $time = 0;
    $baseTime = 0.0;
    $stdsingle = 0.0;
    my $ii = 1;
    # if we're only using real times, don't bother with the corner-cases
    if ($realtimes) {
        $ii = 8;
    }
    for (; $ii <= 16; $ii++) {
        system("randomgen $ii $ii $fileSize");
        if ($realtimes) {
            $time += CheckTime();
            $baseTime += $stdsingle;
        } else {
            # tests with higher levels of randomness are given 
            # higher priority in timing results
            print STDOUT "trying $ii $ii\n" if $debug;
            $time += 2 * $ii * CheckTime();
            $baseTime += 2 * $ii * $stdsingle;
            if ($ii > 1) {
                print STDOUT "trying 1 $ii\n" if $debug;
                system("randomgen 1 $ii $fileSize");
                $time += $ii * CheckTime();
                $baseTime += $ii * $stdsingle;
                print STDOUT "trying $ii 1\n" if $debug;
                system("randomgen $ii 1 $fileSize");
                $time += $ii * CheckTime();
                $baseTime += $ii * $stdsingle;
            }
        }
    }
    if ($time == 0.0) {
        $time = 0.01;
    }
    return $time;
}

# Tests a range of potential values for a variable, and sets it to the fastest.
sub TuneVariable {
    my ($tunevar, $beginval, $endval) = @_;
    my $best_val = $$tunevar;
    my $besttime = 0;
    my $startval = $$tunevar;
    for ($$tunevar = $beginval; $$tunevar <= $endval; $$tunevar++) {
        WriteConstants();
        $sumtime = SumTimes();
        # If this value is better, use it.  If this is the start value
        # and it's just as good, use the startval
        if (not $besttime or ($sumtime < $besttime) or (($besttime == $sumtime) and ($$tunevar == $startval))) {
            $besttime = $sumtime;
            $best_val = $$tunevar;
        }
        print STDOUT "Value: $$tunevar Time: $sumtime\n" if $verbose;
    }
    $$tunevar = $best_val;
    print STDOUT "Best Value: $best_val\n";
    if ($best_val != $startval) {
        $changed = 1;
    }
}

# Determine the cutoff size below which std::sort is faster.
sub TuneMinSize {
    for (; $min_sort_size <= $defFileSize; $min_sort_size *= 2) {
        $loopCount = ($defFileSize/$min_sort_size)/10;
        $fileSize = $min_sort_size;
        WriteConstants();
        $std = "";
        $sumtime = SumTimes();
        $std = "-std";
        $stdtime = SumTimes();
        print STDOUT "Size: $min_sort_size boost::sort Time: $sumtime std::sort Time: $stdtime\n";
        last if ($stdtime > $sumtime);
    }
}
