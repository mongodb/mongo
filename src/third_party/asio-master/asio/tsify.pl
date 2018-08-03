#!/usr/bin/perl -w

use strict;
use File::Path;
# use Switch;

our $output_dir = "tsified";

sub print_line
{
  my ($output, $line, $from, $lineno) = @_;

  # Warn if the resulting line is >80 characters wide.
  if (length($line) > 80)
  {
    if ($from =~ /\.[chi]pp$/)
    {
      print("Warning: $from:$lineno: output >80 characters wide.\n");
    }
  }

  # Write the output.
  print($output $line . "\n");
}

sub source_contains_asio_thread_usage
{
  my ($from) = @_;

  # Open the input file.
  open(my $input, "<$from") or die("Can't open $from for reading");

  # Check file for use of asio::thread.
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /asio::thread/)
    {
      close($input);
      return 1;
    }
    elsif ($line =~ /^ *thread /)
    {
      close($input);
      return 1;
    }
  }

  close($input);
  return 0;
}

sub source_contains_asio_include
{
  my ($from) = @_;

  # Open the input file.
  open(my $input, "<$from") or die("Can't open $from for reading");

  # Check file for inclusion of asio.hpp.
  while (my $line = <$input>)
  {
    chomp($line);
    if ($line =~ /# *include [<"]asio\.hpp[>"]/)
    {
      close($input);
      return 1;
    }
  }

  close($input);
  return 0;
}

sub copy_source_file
{
  my ($from, $to) = @_;

  # Ensure the output directory exists.
  my $dir = $to;
  $dir =~ s/[^\/]*$//;
  mkpath($dir);

  # First determine whether the file makes any use of asio::thread.
  my $uses_asio_thread = source_contains_asio_thread_usage($from);

  my $includes_asio = source_contains_asio_include($from);

  my $is_asio_hpp = 0;
  $is_asio_hpp = 1 if ($from =~ /asio\.hpp/);

  my $needs_doc_link = 0;
  $needs_doc_link = 1 if ($is_asio_hpp);

  my $is_config_hpp = 0;
  $is_config_hpp = 1 if ($from =~ /asio\/detail\/config\.hpp/);

  my $is_error_hpp = 0;
  $is_error_hpp = 1 if ($from =~ /asio\/error\.hpp/);

  my $is_impl_src_hpp = 0;
  $is_impl_src_hpp = 1 if ($from =~ /asio\/impl\/src\.hpp/);

  my $is_test = 0;
  $is_test = 1 if ($from =~ /tests\/unit/);

  # Open the files.
  open(my $input, "<$from") or die("Can't open $from for reading");
  open(my $output, ">$to") or die("Can't open $to for writing");

  # State for stripping out deprecated, extension, and old services code.
  my $deprecated_state = 0;
  my $extension_state = 0;
  my $old_services_state = 0;

  # State for simplifying namespaces in examples.
  my $code_snippet_state = 0;

  # Copy the content.
  my $lineno = 1;
  while (my $line = <$input>)
  {
    chomp($line);

    # Strip out deprecated code.
    if ($deprecated_state == 0)
    {
      if ($line =~ /#\s*if\s*defined\(ASIO_NO_DEPRECATED\)/)
      {
        $deprecated_state = 1;
        next;
      }
      elsif ($line =~ /#\s*if\s*!defined\(ASIO_NO_DEPRECATED\)/)
      {
        $deprecated_state = -1;
        next;
      }
    }
    elsif ($deprecated_state == 1)
    {
      if ($line =~ /#\s*else\s*\/\/\s*!*defined\(ASIO_NO_DEPRECATED\)/)
      {
        $deprecated_state = -1;
        next;
      }
      elsif ($line =~ /#\s*endif\s*\/\/\s*!*defined\(ASIO_NO_DEPRECATED\)/)
      {
        $deprecated_state = 0;
        next;
      }
      else
      {
        $line =~ s/^# /#/;
      }
    }
    elsif ($deprecated_state == -1)
    {
      if ($line =~ /#\s*else\s*\/\/\s*!*defined\(ASIO_NO_DEPRECATED\)/)
      {
        $deprecated_state = 1;
      }
      elsif ($line =~ /#\s*endif\s*\/\/\s*!*defined\(ASIO_NO_DEPRECATED\)/)
      {
        $deprecated_state = -2;
      }
      next;
    }
    elsif ($deprecated_state == -2)
    {
      $deprecated_state = 0;
      next if ($line eq "");
    }

    # Strip out code for extensions.
    if ($extension_state == 0)
    {
      if ($line =~ /#\s*if\s*!defined\(ASIO_NO_EXTENSIONS\)\s*$/)
      {
        $extension_state = -1;
        next;
      }
    }
    elsif ($extension_state == -1)
    {
      $extension_state = -2 if ($line =~ /#\s*endif\s*\/\/\s*!defined\(ASIO_NO_EXTENSIONS\)\s*$/);
      next;
    }
    elsif ($extension_state == -2)
    {
      $extension_state = 0;
      next if ($line eq "");
    }

    # Strip out code for old services.
    if ($old_services_state == 0)
    {
      if ($line =~ /#\s*if\s*defined\(ASIO_ENABLE_OLD_SERVICES\)/)
      {
        $old_services_state = -1;
        next;
      }
      elsif ($line =~ /#\s*if\s*!defined\(ASIO_ENABLE_OLD_SERVICES\)/)
      {
        $old_services_state = 1;
        next;
      }
    }
    elsif ($old_services_state == 1)
    {
      if ($line =~ /#\s*else\s*\/\/\s*!*defined\(ASIO_ENABLE_OLD_SERVICES\)/)
      {
        $old_services_state = -1;
        next;
      }
      elsif ($line =~ /#\s*endif\s*\/\/\s*!*defined\(ASIO_ENABLE_OLD_SERVICES\)/)
      {
        $old_services_state = 0;
        next;
      }
      else
      {
        $line =~ s/^# /#/;
      }
    }
    elsif ($old_services_state == -1)
    {
      if ($line =~ /#\s*else\s*\/\/\s*!*defined\(ASIO_ENABLE_OLD_SERVICES\)/)
      {
        $old_services_state = 1;
      }
      elsif ($line =~ /#\s*endif\s*\/\/\s*!*defined\(ASIO_ENABLE_OLD_SERVICES\)/)
      {
        $old_services_state = -2;
      }
      next;
    }
    elsif ($old_services_state == -2)
    {
      $old_services_state = 0;
      next if ($line eq "");
    }

    # Keep track of whether we are in an example.
    if ($code_snippet_state == 0)
    {
      if ($line =~ /\@code/)
      {
        $code_snippet_state = 1;
      }
    }

    # Unconditional replacements.
    $line =~ s/[\\@]ref boost_bind/std::bind()/g;
    if ($from =~ /.*\.txt$/)
    {
      $line =~ s/[\\@]ref async_read/std::experimental::net::v1::async_read()/g;
      $line =~ s/[\\@]ref async_write/std::experimental::net::v1::async_write()/g;
    }
    if ($line =~ /asio_detail_posix_thread_function/)
    {
      $line =~ s/asio_detail_posix_thread_function/networking_ts_detail_posix_thread_function/g;
    }
    if ($line =~ /asio_signal_handler/)
    {
      $line =~ s/asio_signal_handler/networking_ts_signal_handler/g;
    }
    if ($line =~ /ASIO_/ && !($line =~ /NET_TS_/))
    {
      $line =~ s/ASIO_/NET_TS_/g;
    }
    if ($line =~ /asio_handler_/)
    {
      $line =~ s/asio_handler_/networking_ts_handler_/g;
    }
    if ($line =~ /asio_true_handler_/)
    {
      $line =~ s/asio_true_handler_/networking_ts_true_handler_/g;
    }

    # Lines skipped in asio/impl/src.hpp.
    if ($is_impl_src_hpp)
    {
      next if
        (
             $line =~ /serial_port/
          or $line =~ /\/local\//
          or $line =~ /\/generic\//
          or $line =~ /signal_set/
        );
    }

    # Conditional replacements.
    if ($line =~ /^(.* *)namespace asio \{/)
    {
      print_line($output, $1 . "namespace std {", $from, $lineno);
      print_line($output, $1 . "namespace experimental {", $from, $lineno);
      print_line($output, $1 . "namespace net {", $from, $lineno);
      print_line($output, $1 . "inline namespace v1 {", $from, $lineno);
    }
    elsif ($line =~ /^(.* *)} \/\/ namespace asio$/)
    {
      print_line($output, $1 . "} // inline namespace v1", $from, $lineno);
      print_line($output, $1 . "} // namespace net", $from, $lineno);
      print_line($output, $1 . "} // namespace experimental", $from, $lineno);
      print_line($output, $1 . "} // namespace std", $from, $lineno);
    }
    elsif ($line =~ /^(# *include )[<"](asio\.hpp)[>"]$/)
    {
      print_line($output, $1 . "<net>", $from, $lineno);
      if ($uses_asio_thread)
      {
        print_line($output, $1 . "<thread>", $from, $lineno) if (!$is_test);
        $uses_asio_thread = 0;
      }
    }
    elsif ($line =~ /^(# *include )[<"]boost\/.*[>"].*$/)
    {
      if (!$includes_asio && $uses_asio_thread)
      {
        print_line($output, $1 . "<thread>", $from, $lineno) if (!$is_test);
        $uses_asio_thread = 0;
      }
      print_line($output, $line, $from, $lineno);
    }
    elsif ($line =~ /^(# *include )[<"]asio\/thread\.hpp[>"]/)
    {
      if ($is_test)
      {
        print_line($output, $1 . "<experimental/__net_ts/detail/thread.hpp>", $from, $lineno);
      }
      else
      {
        # Line is removed.
      }
    }
    elsif ($line =~ /(# *include )[<"]asio\/error_code\.hpp[>"]/)
    {
      if ($is_asio_hpp)
      {
        # Line is removed.
      }
      else
      {
        print_line($output, $1 . "<cerrno>", $from, $lineno) if ($is_error_hpp);
        print_line($output, $1 . "<system_error>", $from, $lineno);
      }
    }
    elsif ($line =~ /# *include [<"]asio\/impl\/error_code\.[hi]pp[>"]/)
    {
      # Line is removed.
    }
    elsif ($line =~ /(# *include )[<"]asio\/system_error\.hpp[>"]/)
    {
      if ($is_asio_hpp)
      {
        # Line is removed.
      }
      else
      {
        print_line($output, $1 . "<system_error>", $from, $lineno);
      }
    }
    elsif ($line =~ /(^.*# *include )[<"]asio\/([^>"]*)[>"](.*)$/)
    {
      print_line($output, $1 . "<experimental/__net_ts/" . $2 . ">" . $3, $from, $lineno);
    }
    elsif ($line =~ /#.*defined\(.*ASIO_HAS_STD_SYSTEM_ERROR\)$/)
    {
      # Line is removed.
    }
    elsif ($line =~ /asio::thread\b/)
    {
      if ($is_test)
      {
        $line =~ s/asio::thread/std::experimental::net::v1::detail::thread/g;
      }
      else
      {
        $line =~ s/asio::thread/std::thread/g;
      }
      print_line($output, $line, $from, $lineno);
    }
    elsif ($line =~ /^( *)thread( .*)$/)
    {
      if ($is_test)
      {
        print_line($output, $1 . "std::experimental::net::v1::detail::thread" . $2, $from, $lineno);
      }
      else
      {
        print_line($output, $1 . "std::thread" . $2, $from, $lineno);
      }
    }
    elsif ($line =~ /asio::/ && !($line =~ /boost::asio::/))
    {
      $line =~ s/asio::error_code/std::error_code/g;
      $line =~ s/asio::error_category/std::error_category/g;
      $line =~ s/asio::system_category/std::system_category/g;
      $line =~ s/asio::system_error/std::system_error/g;
      $line =~ s/asio::/std::experimental::net::v1::/g if $code_snippet_state == 0;
      $line =~ s/asio::/std::experimental::net::/g if $code_snippet_state == 1;
      print_line($output, $line, $from, $lineno);
    }
    elsif ($line =~ /using namespace asio/)
    {
      $line =~ s/using namespace asio/using namespace std::experimental::net::v1/g;
      print_line($output, $line, $from, $lineno);
    }
    elsif ($line =~ /[\\@]ref boost_bind/)
    {
      $line =~ s/[\\@]ref boost_bind/std::bind()/g;
      print_line($output, $line, $from, $lineno);
    }
    elsif ($line =~ /define.*DETAIL_CONFIG_HPP/)
    {
      print_line($output, $line, $from, $lineno);
      print_line($output, "", $from, $lineno);
      print_line($output, "#define NET_TS_STANDALONE 1", $from, $lineno);
    }
    else
    {
      print_line($output, $line, $from, $lineno);
    }

    # Keep track of whether we are in an example.
    if ($code_snippet_state == 1)
    {
      if ($line =~ /\@endcode/)
      {
        $code_snippet_state = 0;
      }
    }

    ++$lineno;
  }

  # Ok, we're done.
  close($input);
  close($output);
}

sub find_include_files
{
  my @root_includes = (
      "asio/ts/buffer.hpp",
      "asio/ts/executor.hpp",
      "asio/ts/internet.hpp",
      "asio/ts/io_context.hpp",
      "asio/ts/net.hpp",
      "asio/ts/netfwd.hpp",
      "asio/ts/socket.hpp",
      "asio/ts/timer.hpp");

  my @excluded_includes = (
      "asio/basic_deadline_timer.hpp",
      "asio/basic_streambuf.hpp",
      "asio/basic_streambuf_fwd.hpp",
      "asio/datagram_socket_service.hpp",
      "asio/deadline_timer.hpp",
      "asio/deadline_timer_service.hpp",
      "asio/io_context_strand.hpp",
      "asio/detail/strand_service.hpp",
      "asio/detail/impl/strand_service.hpp",
      "asio/detail/impl/strand_service.ipp",
      "asio/socket_acceptor_service.hpp",
      "asio/seq_packet_socket_service.hpp",
      "asio/stream_socket_service.hpp",
      "asio/time_traits.hpp",
      "asio/waitable_timer_service.hpp");

  my @include_files = ();
  my %known_includes = ();

  foreach my $include (@root_includes)
  {
    $known_includes{$include} = 1;
    push(@include_files, "include/" . $include);
  }

  foreach my $include (@excluded_includes)
  {
    $known_includes{$include} = 1;
  }

  my @unprocessed_includes = sort keys %known_includes;
  while (scalar(@unprocessed_includes) > 0)
  {
    my $include = pop(@unprocessed_includes);

    open(my $input, "<include/$include") or die("Can't open include/$include for reading");
    while (my $line = <$input>)
    {
      chomp($line);
      if ($line =~ /^\s*#\s*include\s*"(asio\/[^"]*)"/)
      {
        if (not defined($known_includes{$1}))
        {
          $known_includes{$1} = 1;
          push(@include_files, "include/" . $1);
          push(@unprocessed_includes, $1);
        }
      }
    }
    close($input);
  }

  return @include_files;
}

sub copy_include_files
{
  my @files = find_include_files();
  push(@files, "include/asio/impl/src.hpp");

  foreach my $file (@files)
  {
    if ($file ne "include/asio/thread.hpp"
        and $file ne "include/asio/error_code.hpp"
        and $file ne "include/asio/system_error.hpp"
        and $file ne "include/asio/impl/error_code.hpp"
        and $file ne "include/asio/impl/error_code.ipp")
    {
      my $from = $file;
      my $to = $file;
      $to =~ s/^include\/asio\//$output_dir\/include\/experimental\/__net_ts\//;
      copy_source_file($from, $to);
    }
  }
}

sub create_top_level_includes
{
  my @includes = (
      "buffer",
      "executor",
      "internet",
      "io_context",
      "net",
      "netfwd",
      "socket",
      "timer");

  our $output_dir;
  mkpath("$output_dir/include/experimental");
  foreach my $include (@includes)
  {
    open(my $output, ">$output_dir/include/experimental/$include")
      or die("Can't open $output_dir/include/experimental/$include for writing");

    my $guard = "NET_TS_" . uc($include) . "_INCLUDED";

    print($output "//\n");
    print($output "// experimental/$include\n");
    print($output "// " . '~' x length("experimental/" . $include) . "\n");
    print($output "//\n");
    print($output "// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)\n");
    print($output "//\n");
    print($output "// Distributed under the Boost Software License, Version 1.0. (See accompanying\n");
    print($output "// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)\n");
    print($output "//\n");
    print($output "\n");
    print($output "#ifndef $guard\n");
    print($output "#define $guard\n");
    print($output "\n");
    print($output "#include <experimental/__net_ts/ts/$include.hpp>\n");
    print($output "\n");
    print($output "#endif // $guard\n");

    close($output);
  }
}

copy_include_files();
create_top_level_includes();
