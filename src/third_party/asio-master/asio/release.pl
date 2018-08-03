#!/usr/bin/perl -w

use strict;
use Cwd qw(abs_path getcwd);
use Date::Format;
use File::Path;
use File::Copy;
use File::Basename;

our $version_major;
our $version_minor;
our $version_sub_minor;
our $asio_name;
our $boost_asio_name;

sub print_usage_and_exit
{
  print("usage: ./release.pl <version>\n");
  print("   or: ./release.pl --package-asio\n");
  print("\n");
  print("examples:\n");
  print("  create new version and build packages for asio and boost.asio:\n");
  print("    ./release.pl 1.2.0\n");
  print("  create packages for asio only:\n");
  print("    ./release.pl --package-asio\n");
  exit(1);
}

sub determine_version($)
{
  my $version_string = shift;
  if ($version_string =~ /^([0-9]+)\.([0-9]+)\.([0-9]+)$/)
  {
    our $version_major = $1;
    our $version_minor = $2;
    our $version_sub_minor = $3;

    our $asio_name = "asio";
    $asio_name .= "-$version_major";
    $asio_name .= ".$version_minor";
    $asio_name .= ".$version_sub_minor";

    our $boost_asio_name = "boost_asio";
    $boost_asio_name .= "_$version_major";
    $boost_asio_name .= "_$version_minor";
    $boost_asio_name .= "_$version_sub_minor";
  }
  else
  {
    print_usage_and_exit();
  }
}

sub determine_version_from_configure()
{
  my $from = "configure.ac";
  open(my $input, "<$from") or die("Can't open $from for reading");
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /^AC_INIT\(asio.*\[(.*)\]\)$/)
    {
      our $asio_name = "asio-$1";
      our $boost_asio_name = "boost_asio_$1";
      last;
    }
  }
}

sub update_configure_ac
{
  # Open the files.
  my $from = "configure.ac";
  my $to = $from . ".tmp";
  open(my $input, "<$from") or die("Can't open $from for reading");
  open(my $output, ">$to") or die("Can't open $to for writing");

  # Copy the content.
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /^AC_INIT\(asio.*\)$/)
    {
      $line = "AC_INIT(asio, [";
      $line .= "$version_major.$version_minor.$version_sub_minor";
      $line .= "])";
    }
    print($output "$line\n");
  }

  # Close the files and move the temporary output into position.
  close($input);
  close($output);
  move($to, $from);
  unlink($to);
}

sub update_readme
{
  # Open the files.
  my $from = "README";
  my $to = $from . ".tmp";
  open(my $input, "<$from") or die("Can't open $from for reading");
  open(my $output, ">$to") or die("Can't open $to for writing");

  # Copy the content.
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /^asio version/)
    {
      $line = "asio version ";
      $line .= "$version_major.$version_minor.$version_sub_minor";
    }
    elsif ($line =~ /^Released/)
    {
      my @time = localtime;
      $line = "Released " . strftime("%A, %d %B %Y", @time) . ".";
    }
    print($output "$line\n");
  }

  # Close the files and move the temporary output into position.
  close($input);
  close($output);
  move($to, $from);
  unlink($to);
}

sub update_asio_version_hpp
{
  # Open the files.
  my $from = "include/asio/version.hpp";
  my $to = $from . ".tmp";
  open(my $input, "<$from") or die("Can't open $from for reading");
  open(my $output, ">$to") or die("Can't open $to for writing");

  # Copy the content.
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /^#define ASIO_VERSION /)
    {
      my $version = $version_major * 100000;
      $version += $version_minor * 100;
      $version += $version_sub_minor + 0;
      $line = "#define ASIO_VERSION " . $version;
      $line .= " // $version_major.$version_minor.$version_sub_minor";
    }
    print($output "$line\n");
  }

  # Close the files and move the temporary output into position.
  close($input);
  close($output);
  move($to, $from);
  unlink($to);
}

sub update_boost_asio_version_hpp
{
  # Open the files.
  my $from = "../boost/boost/asio/version.hpp";
  my $to = $from . ".tmp";
  open(my $input, "<$from") or die("Can't open $from for reading");
  open(my $output, ">$to") or die("Can't open $to for writing");

  # Copy the content.
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /^#define BOOST_ASIO_VERSION /)
    {
      my $version = $version_major * 100000;
      $version += $version_minor * 100;
      $version += $version_sub_minor + 0;
      $line = "#define BOOST_ASIO_VERSION " . $version;
      $line .= " // $version_major.$version_minor.$version_sub_minor";
    }
    print($output "$line\n");
  }

  # Close the files and move the temporary output into position.
  close($input);
  close($output);
  move($to, $from);
  unlink($to);
}

sub build_asio_doc
{
  $ENV{BOOST_ROOT} = abs_path("../boost");
  system("rm -rf doc");
  my $bjam = abs_path(glob("../boost/bjam"));
  chdir("src/doc");
  system("$bjam clean");
  system("rm -rf html");
  system("$bjam");
  chdir("../..");
  mkdir("doc");
  system("cp -vR src/doc/html/* doc");
}

sub build_example_diffs
{
  my @cpp11_files = `find src/examples/cpp11 -type f -name "*.*pp"`;
  foreach my $cpp11_file (@cpp11_files)
  {
    chomp($cpp11_file);

    my $cpp03_file = $cpp11_file;
    $cpp03_file =~ s/\/cpp11\//\/cpp03\//;
    my $output_diff = $cpp11_file;
    $output_diff =~ s/src\/examples\/cpp11\///g;
    my ($output_diff_name, $output_dir) = fileparse($output_diff);
    my $output_html = $output_diff . ".html";

    mkpath("doc/examples/diffs/$output_dir");
    system("diff -U1000000 $cpp03_file $cpp11_file > doc/examples/diffs/$output_diff");
    system("cd doc/examples/diffs && diff2html.py -i $output_diff -o $output_html");
    unlink("doc/examples/diffs/$output_diff");
  }
}

sub make_asio_packages
{
  our $asio_name;
  system("./autogen.sh");
  system("./configure");
  system("make dist");
  system("tar tfz $asio_name.tar.gz | sed -e 's/^[^\\/]*//' | sort -df > asio.manifest");
}

sub build_boost_asio_doc
{
  my $cwd = getcwd();
  my $bjam = abs_path(glob("../boost/bjam"));
  chdir("../boost/doc");
  system("rm -rf html/boost_asio");
  chdir("../libs/asio/doc");
  system("$bjam clean");
  system("$bjam asio");
  chdir($cwd);
}

our $boost_asio_readme = <<"EOF";
Copy the `boost', `doc' and `libs' directories into an existing boost 1.33.0,
1.33.1, 1.34, 1.34.1, 1.35 or 1.36 distribution.

Before using Boost.Asio, the Boost.System library needs to be built. This can
be done by running bjam in the libs/system/build directory. Consult the Boost
Getting Started page (http://www.boost.org/more/getting_started.html) for more
information on how to build the Boost libraries.
EOF

our $boost_system_jamfile = <<"EOF";
# Boost System Library Build Jamfile

# (C) Copyright Beman Dawes 2002, 2006

# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or www.boost.org/LICENSE_1_0.txt)

# See library home page at http://www.boost.org/libs/system

subproject libs/system/build ;

SOURCES = error_code ;

lib boost_system
     : ../src/$(SOURCES).cpp
     : # build requirements
      <define>BOOST_SYSTEM_STATIC_LINK
      <sysinclude>$(BOOST_AUX_ROOT) <sysinclude>$(BOOST_ROOT)
      # common-variant-tag ensures that the library will
      # be named according to the rules used by the install
      # and auto-link features:
      common-variant-tag 
     : debug release  # build variants
     ;

dll boost_system
     : ../src/$(SOURCES).cpp
     : # build requirements
       <define>BOOST_SYSTEM_DYN_LINK=1  # tell source we're building dll's
       <runtime-link>dynamic  # build only for dynamic runtimes
       <sysinclude>$(BOOST_AUX_ROOT) <sysinclude>$(BOOST_ROOT)
      # common-variant-tag ensures that the library will
      # be named according to the rules used by the install
      # and auto-link features:
      common-variant-tag 
     : debug release  # build variants
     ;

install system lib
     : <lib>boost_system <dll>boost_system
     ;

stage stage/lib : <lib>boost_system <dll>boost_system
    :
        # copy to a path rooted at BOOST_ROOT:
        <locate>$(BOOST_ROOT)
        # make sure the names of the libraries are correctly named:
        common-variant-tag
        # add this target to the "stage" and "all" psuedo-targets:
        <target>stage
        <target>all
    :
        debug release
    ;

# end
EOF

sub create_boost_asio_content
{
  # Create directory structure.
  system("rm -rf $boost_asio_name");
  mkdir("$boost_asio_name");
  mkdir("$boost_asio_name/doc");
  mkdir("$boost_asio_name/doc/html");
  mkdir("$boost_asio_name/boost");
  mkdir("$boost_asio_name/boost/config");
  mkdir("$boost_asio_name/libs");

  # Copy files.
  system("cp -vLR ../boost/doc/html/boost_asio.html $boost_asio_name/doc/html");
  system("cp -vLR ../boost/doc/html/boost_asio $boost_asio_name/doc/html");
  system("cp -vLR ../boost/boost/asio.hpp $boost_asio_name/boost");
  system("cp -vLR ../boost/boost/asio $boost_asio_name/boost");
  system("cp -vLR ../boost/boost/cerrno.hpp $boost_asio_name/boost");
  system("cp -vLR ../boost/boost/config/warning_disable.hpp $boost_asio_name/boost/config");
  system("cp -vLR ../boost/boost/system $boost_asio_name/boost");
  system("cp -vLR ../boost/libs/asio $boost_asio_name/libs");
  system("cp -vLR ../boost/libs/system $boost_asio_name/libs");

  # Remove modular boost include directories.
  system("rm -rf $boost_asio_name/libs/asio/include");
  system("rm -rf $boost_asio_name/libs/system/include");

  # Add dummy definitions of BOOST_SYMBOL* to boost/system/config.hpp.
  my $from = "$boost_asio_name/boost/system/config.hpp";
  my $to = "$boost_asio_name/boost/system/config.hpp.new";
  open(my $input, "<$from") or die("Can't open $from for reading");
  open(my $output, ">$to") or die("Can't open $to for writing");
  while (my $line = <$input>)
  {
    print($output $line);
    if ($line =~ /<boost\/config\.hpp>/)
    {
      print($output "\n// These #defines added by the separate Boost.Asio package.\n");
      print($output "#if !defined(BOOST_SYMBOL_IMPORT)\n");
      print($output "# if defined(BOOST_HAS_DECLSPEC)\n");
      print($output "#  define BOOST_SYMBOL_IMPORT __declspec(dllimport)\n");
      print($output "# else // defined(BOOST_HAS_DECLSPEC)\n");
      print($output "#  define BOOST_SYMBOL_IMPORT\n");
      print($output "# endif // defined(BOOST_HAS_DECLSPEC)\n");
      print($output "#endif // !defined(BOOST_SYMBOL_IMPORT)\n");
      print($output "#if !defined(BOOST_SYMBOL_EXPORT)\n");
      print($output "# if defined(BOOST_HAS_DECLSPEC)\n");
      print($output "#  define BOOST_SYMBOL_EXPORT __declspec(dllexport)\n");
      print($output "# else // defined(BOOST_HAS_DECLSPEC)\n");
      print($output "#  define BOOST_SYMBOL_EXPORT\n");
      print($output "# endif // defined(BOOST_HAS_DECLSPEC)\n");
      print($output "#endif // !defined(BOOST_SYMBOL_EXPORT)\n");
      print($output "#if !defined(BOOST_SYMBOL_VISIBLE)\n");
      print($output "# define BOOST_SYMBOL_VISIBLE\n");
      print($output "#endif // !defined(BOOST_SYMBOL_VISIBLE)\n\n");
    }
  }
  close($input);
  close($output);
  system("mv $to $from");

  # Create readme.
  $to = "$boost_asio_name/README.txt";
  open($output, ">$to") or die("Can't open $to for writing");
  print($output $boost_asio_readme);
  close($output);

  # Create Boost.System Jamfile.
  $to = "$boost_asio_name/libs/system/build/Jamfile";
  open($output, ">$to") or die("Can't open $to for writing");
  print($output $boost_system_jamfile);
  close($output);

  # Remove SVN and git files.
  system("find $boost_asio_name -name .svn -exec rm -rf {} \\;");
  system("find $boost_asio_name -name .git -exec rm -rf {} \\;");
  system("find $boost_asio_name -name .gitignore -exec rm -rf {} \\;");
  system("find $boost_asio_name -name .gitattributes -exec rm -rf {} \\;");
}

sub make_boost_asio_packages
{
  our $boost_asio_name;
  system("tar --format=ustar -chf - $boost_asio_name | gzip -c >$boost_asio_name.tar.gz");
  system("tar --format=ustar -chf - $boost_asio_name | bzip2 -9 -c >$boost_asio_name.tar.bz2");
  system("rm -f $boost_asio_name.zip");
  system("zip -rq $boost_asio_name.zip $boost_asio_name");
  system("rm -rf $boost_asio_name");
  system("tar tfz $boost_asio_name.tar.gz | sed -e 's/^[^\\/]*//' | sort -df > boost_asio.manifest");
}

(scalar(@ARGV) == 1) or print_usage_and_exit();
my $new_version = 1;
my $package_asio = 1;
my $package_boost = 1;
if ($ARGV[0] eq "--package-asio")
{
  $new_version = 0;
  $package_boost = 0;
}

if ($new_version)
{
  determine_version($ARGV[0]);
  update_configure_ac();
  update_readme();
  update_asio_version_hpp();
  update_boost_asio_version_hpp();
}
else
{
  determine_version_from_configure();
}

if ($package_asio)
{
  build_asio_doc();
  build_example_diffs();
  make_asio_packages();
}

if ($package_boost)
{
  build_boost_asio_doc();
  create_boost_asio_content();
  make_boost_asio_packages();
}
