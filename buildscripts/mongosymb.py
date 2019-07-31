#!/usr/bin/env python3
"""Script and library for symbolizing MongoDB stack traces.

To use as a script, paste the JSON object on the line after ----- BEGIN BACKTRACE ----- into the
standard input of this script. There are numerous caveats. In the default mode, you need
to pass in the path to the executable being symbolized, and if you want shared library stack
traces, you must be on the same system.

There is largely untested support for extracting debug information from S3 buckets. This work
is experimental.

Sample usage:

mongosymb.py --symbolizer-path=/path/to/llvm-symbolizer /path/to/executable </file/with/stacktrace

You can also pass --output-format=json, to get rich json output. It shows some extra information,
but emits json instead of plain text.
"""

import json
import optparse
import os
import subprocess
import sys


def parse_input(trace_doc, dbg_path_resolver):
    """Return a list of frame dicts from an object of {backtrace: list(), processInfo: dict()}."""

    def make_base_addr_map(somap_list):
        """Return map from binary load address to description of library from the somap_list.

        The somap_list is a list of dictionaries describing individual loaded libraries.
        """
        return {so_entry["b"]: so_entry for so_entry in somap_list if "b" in so_entry}

    base_addr_map = make_base_addr_map(trace_doc["processInfo"]["somap"])

    frames = []
    for frame in trace_doc["backtrace"]:
        soinfo = base_addr_map.get(frame["b"], {})
        elf_type = soinfo.get("elfType", 0)
        if elf_type == 3:
            addr_base = "0"
        elif elf_type == 2:
            addr_base = frame["b"]
        else:
            addr_base = soinfo.get("vmaddr", "0")
        addr = int(addr_base, 16) + int(frame["o"], 16)
        # addr currently points to the return address which is the one *after* the call. x86 is
        # variable length so going backwards is difficult. However llvm-symbolizer seems to do the
        # right thing if we just subtract 1 byte here. This has the downside of also adjusting the
        # address of instructions that cause signals (such as segfaults and divide-by-zero) which
        # are already correct, but there doesn't seem to be a reliable way to detect that case.
        addr -= 1
        frames.append(
            dict(
                path=dbg_path_resolver.get_dbg_file(soinfo), buildId=soinfo.get("buildId", None),
                offset=frame["o"], addr="0x{:x}".format(addr), symbol=frame.get("s", None)))
    return frames


def symbolize_frames(trace_doc, dbg_path_resolver, symbolizer_path, dsym_hint, input_format,
                     **_kwargs):
    """Return a list of symbolized stack frames from a trace_doc in MongoDB stack dump format."""

    if not symbolizer_path:
        symbolizer_path = os.environ.get("MONGOSYMB_SYMBOLIZER_PATH", "llvm-symbolizer")

    if input_format == "classic":
        frames = parse_input(trace_doc, dbg_path_resolver)
    elif input_format == "thin":
        frames = trace_doc["backtrace"]
        for frame in frames:
            frame["path"] = dbg_path_resolver.get_dbg_file(frame)
    else:
        raise ValueError('Unknown input format "{}"'.format(input_format))

    symbolizer_args = [symbolizer_path]
    for dh in dsym_hint:
        symbolizer_args.append("-dsym-hint=%s" % dh)
    symbolizer_process = subprocess.Popen(args=symbolizer_args, close_fds=True,
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=open("/dev/null"))

    def extract_symbols(stdin):
        """Extract symbol information from the output of llvm-symbolizer.

        Return a list of dictionaries, each of which has fn, file, column and line entries.

        The format of llvm-symbolizer output is that for every CODE line of input,
        it outputs zero or more pairs of lines, and then a blank line. This way, if
        a CODE line of input maps to several inlined functions, you can use the blank
        line to find the end of the list of symbols corresponding to the CODE line.

        The first line of each pair contains the function name, and the second contains the file,
        column and line information.
        """
        result = []
        step = 0
        while True:
            line = stdin.readline().decode()
            if line == "\n":
                break
            if step == 0:
                result.append({"fn": line.strip()})
                step = 1
            else:
                file_name, line, column = line.strip().rsplit(':', 3)
                result[-1].update({"file": file_name, "column": int(column), "line": int(line)})
                step = 0
        return result

    for frame in frames:
        if frame["path"] is None:
            continue
        symbol_line = "CODE {path:} {addr:}\n".format(**frame)
        symbolizer_process.stdin.write(symbol_line.encode())
        symbolizer_process.stdin.flush()
        frame["symbinfo"] = extract_symbols(symbolizer_process.stdout)
    symbolizer_process.stdin.close()
    symbolizer_process.wait()
    return frames


class PathDbgFileResolver(object):
    """PathDbgFileResolver class."""

    def __init__(self, bin_path_guess):
        """Initialize PathDbgFileResolver."""
        self._bin_path_guess = os.path.realpath(bin_path_guess)

    def get_dbg_file(self, soinfo):
        """Return dbg file name."""
        path = soinfo.get("path", "")
        return path if path else self._bin_path_guess


class S3BuildidDbgFileResolver(object):
    """S3BuildidDbgFileResolver class."""

    def __init__(self, cache_dir, s3_bucket):
        """Initialize S3BuildidDbgFileResolver."""
        self._cache_dir = cache_dir
        self._s3_bucket = s3_bucket

    def get_dbg_file(self, soinfo):
        """Return dbg file name."""
        build_id = soinfo.get("buildId", None)
        if build_id is None:
            return None
        build_id = build_id.lower()
        build_id_path = os.path.join(self._cache_dir, build_id + ".debug")
        if not os.path.exists(build_id_path):
            try:
                self._get_from_s3(build_id)
            except Exception:  # pylint: disable=broad-except
                ex = sys.exc_info()[0]
                sys.stderr.write("Failed to find debug symbols for %s in s3: %s\n" % (build_id, ex))
                return None
        if not os.path.exists(build_id_path):
            return None
        return build_id_path

    def _get_from_s3(self, build_id):
        """Download debug symbols from S3."""
        subprocess.check_call(
            ['wget',
             'https://s3.amazonaws.com/%s/%s.debug.gz' % (self._s3_bucket, build_id)],
            cwd=self._cache_dir)
        subprocess.check_call(['gunzip', build_id + ".debug.gz"], cwd=self._cache_dir)


def classic_output(frames, outfile, **kwargs):  # pylint: disable=unused-argument
    """Provide classic output."""
    for frame in frames:
        symbinfo = frame["symbinfo"]
        if symbinfo:
            for sframe in symbinfo:
                outfile.write(" %(file)s:%(line)s:%(column)s: %(fn)s\n" % sframe)
        else:
            outfile.write(" %(path)s!!!\n" % symbinfo)


def main(argv):
    """Execute Main program."""
    parser = optparse.OptionParser()
    parser.add_option("--dsym-hint", action="append", dest="dsym_hint", default=list())
    parser.add_option("--symbolizer-path", dest="symbolizer_path", default=str())
    parser.add_option("--debug-file-resolver", dest="debug_file_resolver", default="path")
    parser.add_option("--input-format", dest="input_format", default="classic")
    parser.add_option("--output-format", dest="output_format", default="classic")
    (options, args) = parser.parse_args(argv)
    resolver_constructor = dict(path=PathDbgFileResolver, s3=S3BuildidDbgFileResolver).get(
        options.debug_file_resolver, None)
    if resolver_constructor is None:
        sys.stderr.write("Invalid debug-file-resolver argument: %s\n" % options.debug_file_resolver)
        sys.exit(1)

    output_fn = dict(json=json.dump, classic=classic_output).get(options.output_format, None)
    if output_fn is None:
        sys.stderr.write("Invalid output-format argument: %s\n" % options.output_format)
        sys.exit(1)

    unnamed_args = args[1:]

    # Skip over everything before the first '{' since it is likely to be log line prefixes.
    # Additionally, using raw_decode() to ignore extra data after the closing '}' to allow maximal
    # sloppiness in copy-pasting input.
    trace_doc = sys.stdin.read()
    trace_doc = trace_doc[trace_doc.find('{'):]
    trace_doc = json.JSONDecoder().raw_decode(trace_doc)[0]

    resolver = resolver_constructor(*unnamed_args)
    frames = symbolize_frames(trace_doc, resolver, **vars(options))
    output_fn(frames, sys.stdout, indent=2)


if __name__ == '__main__':
    main(sys.argv)
    sys.exit(0)
