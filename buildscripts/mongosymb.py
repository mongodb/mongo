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
import argparse
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
                     **kwargs):
    """Return a list of symbolized stack frames from a trace_doc in MongoDB stack dump format."""

    # Keep frames in kwargs to avoid changing the function signature.
    frames = kwargs.get("frames")
    if frames is None:
        frames = preprocess_frames(dbg_path_resolver, trace_doc, input_format)

    if not symbolizer_path:
        symbolizer_path = os.environ.get("MONGOSYMB_SYMBOLIZER_PATH", "llvm-symbolizer")

    symbolizer_args = [symbolizer_path]
    for dh in dsym_hint:
        symbolizer_args.append("-dsym-hint={}".format(dh))
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


def preprocess_frames(dbg_path_resolver, trace_doc, input_format):
    """Process the paths in frame objects."""
    if input_format == "classic":
        frames = parse_input(trace_doc, dbg_path_resolver)
    elif input_format == "thin":
        frames = trace_doc["backtrace"]
        for frame in frames:
            frame["path"] = dbg_path_resolver.get_dbg_file(frame)
    else:
        raise ValueError('Unknown input format "{}"'.format(input_format))
    return frames


class PathDbgFileResolver(object):
    """PathDbgFileResolver class."""

    def __init__(self, bin_path_guess):
        """Initialize PathDbgFileResolver."""
        self._bin_path_guess = os.path.realpath(bin_path_guess)
        self.mci_build_dir = None

    def get_dbg_file(self, soinfo):
        """Return dbg file name."""
        path = soinfo.get("path", "")
        # TODO: make identifying mongo shared library directory more robust
        if self.mci_build_dir is None and path.startswith("/data/mci/"):
            self.mci_build_dir = path.split("/src/", maxsplit=1)[0]
        return path if path else self._bin_path_guess


class S3BuildidDbgFileResolver(object):
    """S3BuildidDbgFileResolver class."""

    def __init__(self, cache_dir, s3_bucket):
        """Initialize S3BuildidDbgFileResolver."""
        self._cache_dir = cache_dir
        self._s3_bucket = s3_bucket
        self.mci_build_dir = None

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
                sys.stderr.write("Failed to find debug symbols for {} in s3: {}\n".format(
                    build_id, ex))
                return None
        if not os.path.exists(build_id_path):
            return None
        return build_id_path

    def _get_from_s3(self, build_id):
        """Download debug symbols from S3."""
        subprocess.check_call(
            ['wget', 'https://s3.amazonaws.com/{}/{}.debug.gz'.format(self._s3_bucket, build_id)],
            cwd=self._cache_dir)
        subprocess.check_call(['gunzip', build_id + ".debug.gz"], cwd=self._cache_dir)


def classic_output(frames, outfile, **kwargs):  # pylint: disable=unused-argument
    """Provide classic output."""
    for frame in frames:
        symbinfo = frame["symbinfo"]
        if symbinfo:
            for sframe in symbinfo:
                outfile.write(" {file:s}:{line:d}:{column:d}: {fn:s}\n".format(**sframe))
        else:
            outfile.write(" {path:s}!!!\n".format(**symbinfo))


def make_argument_parser(parser=None, **kwargs):
    """Make and return an argparse."""
    if parser is None:
        parser = argparse.ArgumentParser(**kwargs)

    parser.add_argument('--dsym-hint', default=[], action='append')
    parser.add_argument('--symbolizer-path', default='')
    parser.add_argument('--input-format', choices=['classic', 'thin'], default='classic')
    parser.add_argument('--output-format', choices=['classic', 'json'], default='classic',
                        help='"json" shows some extra information')
    parser.add_argument('--debug-file-resolver', choices=['path', 's3'], default='path')
    parser.add_argument('--src-dir-to-move', action="store", type=str, default=None,
                        help="Specify a src dir to move to /data/mci/{original_buildid}/src")

    s3_group = parser.add_argument_group(
        "s3 options", description='Options used with \'--debug-file-resolver s3\'')
    s3_group.add_argument('--s3-cache-dir')
    s3_group.add_argument('--s3-bucket')

    # Look for symbols in the cwd by default.
    parser.add_argument('path_to_executable', nargs="?")
    return parser


def main(options):
    """Execute Main program."""

    # Skip over everything before the first '{' since it is likely to be log line prefixes.
    # Additionally, using raw_decode() to ignore extra data after the closing '}' to allow maximal
    # sloppiness in copy-pasting input.
    trace_doc = sys.stdin.read()

    if not trace_doc or not trace_doc.strip():
        print("Please provide the backtrace through stdin for symbolization;"
              "e.g. `your/symbolization/command < /file/with/stacktrace`")
    trace_doc = trace_doc[trace_doc.find('{'):]
    trace_doc = json.JSONDecoder().raw_decode(trace_doc)[0]

    # Search the trace_doc for an object having "backtrace" and "processInfo" keys.
    def bt_search(obj):
        try:
            if "backtrace" in obj and "processInfo" in obj:
                return obj
            for _, val in obj.items():
                res = bt_search(val)
                if res:
                    return res
        except (TypeError, AttributeError):
            pass
        return None

    trace_doc = bt_search(trace_doc)

    if not trace_doc:
        print("could not find json backtrace object in input", file=sys.stderr)
        exit(1)

    output_fn = None
    if options.output_format == 'json':
        output_fn = json.dump
    if options.output_format == 'classic':
        output_fn = classic_output

    resolver = None
    if options.debug_file_resolver == 'path':
        resolver = PathDbgFileResolver(options.path_to_executable)
    elif options.debug_file_resolver == 's3':
        resolver = S3BuildidDbgFileResolver(options.s3_cache_dir, options.s3_bucket)

    frames = preprocess_frames(resolver, trace_doc, options.input_format)

    if options.src_dir_to_move and resolver.mci_build_dir is not None:
        try:
            os.makedirs(resolver.mci_build_dir)
            os.symlink(
                os.path.join(os.getcwd(), options.src_dir_to_move),
                os.path.join(resolver.mci_build_dir, 'src'))
        except FileExistsError:
            pass

    frames = symbolize_frames(frames=frames, trace_doc=trace_doc, dbg_path_resolver=resolver,
                              **vars(options))
    output_fn(frames, sys.stdout, indent=2)


if __name__ == '__main__':
    symbolizer_options = make_argument_parser(description=__doc__).parse_args()
    main(symbolizer_options)
    sys.exit(0)
