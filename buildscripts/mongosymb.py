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

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from abc import ABC, abstractmethod
from collections import OrderedDict
from datetime import timedelta
from pathlib import Path
from typing import Dict, List, Any, Union, Optional

import requests

from tenacity import wait_fixed, stop_after_delay, retry_if_result, Retrying

sys.path.append(str(Path(os.getcwd(), __file__).parent.parent))

from buildscripts.util.oauth import Configs, get_oauth_credentials, get_client_cred_oauth_credentials  #pylint: disable=wrong-import-position
from buildscripts.build_system_options import PathOptions  #pylint: disable=wrong-import-position

SYMBOLIZER_PATH_ENV = "MONGOSYMB_SYMBOLIZER_PATH"
# since older versions may have issues with symbolizing, we are setting the toolchain version to v4
DEFAULT_SYMBOLIZER_PATH = "/opt/mongodbtoolchain/v4/bin/llvm-symbolizer"


class DbgFileResolver(ABC):
    """Base gdb path resolver class."""

    @abstractmethod
    def get_dbg_file(self, soinfo: Dict[str, Any]) -> Union[str, None]:
        """
        To get path for given build info.

        :param soinfo: soinfo as dict
        :return: path as string or None (if path not found)
        """
        raise NotImplementedError


class PathDbgFileResolver(DbgFileResolver):
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


class S3BuildidDbgFileResolver(DbgFileResolver):
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
            except Exception:  # noqa pylint: disable=broad-except
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


class CachedResults(object):
    """
    Used to manage / store results in a cache form (using dict as an underlying data structure).

    Idea is to allow only N items to be present in cache at a time and eliminate extra items on the go.
    """

    def __init__(self, max_cache_size: int, initial_cache: Dict[str, str] = None):
        """
        Initialize instance.

        :param max_cache_size: max number of items that can be added to cache
        :param initial_cache: initial items as dict
        """
        self._max_cache_size = max_cache_size
        self._cached_results = OrderedDict(initial_cache or {})

    def insert(self, key: str, value: str) -> Dict[str, str] or None:
        """
        Insert new data into cache.

        :param key: key string
        :param value: value string
        :return: inserted data as dict or None (if not possible to insert)
        """
        if self._max_cache_size <= 0:
            # we can't insert into 0-length dict
            return None

        if len(self._cached_results) >= self._max_cache_size:
            # remove items causing the size overflow of cache
            # we use FIFO order when removing objects from cache,
            # so that we delete olds and keep track of only the recent ones
            keys_iterator = iter(self._cached_results.keys())
            while len(self._cached_results) >= self._max_cache_size:
                # pop the first (the oldest) item in dict
                self._cached_results.pop(next(keys_iterator))

        if key not in self._cached_results:
            # actual insert operation
            self._cached_results[key] = value

        return dict(build_id=value)

    def get(self, key: str) -> str or None:
        """
        Try to get object by key.

        :param key: key string
        :return: value for key
        """
        if self._max_cache_size <= 0:
            return None

        return self._cached_results.get(key)


class PathResolver(DbgFileResolver):
    """
    Class to find path for given buildId.

    We'll be sending request each time to another server to get path.
    This process is fairly small, but can be heavy in case of increased amount of requests per second.
    Thus, I'm implementing a caching mechanism (as a suggestion).
    It keeps track of the last N results from server, we always try to search from that cache, if not found then send
    request to server and cache the response for further usage.
    Cache size differs according to the situation, system resources and overall decision of development team.
    """

    # This amount of attributes are necessary.

    # the main (API) sever that we'll be sending requests to
    default_host = "https://symbolizer-service.server-tig.prod.corp.mongodb.com"
    default_cache_dir = os.path.join(os.getcwd(), "build", "symbolizer_downloads_cache")
    default_creds_file_path = os.path.join(os.getcwd(), ".symbolizer_credentials.json")
    default_client_credentials_scope = "servertig-symbolizer-fullaccess"
    default_client_credentials_user_name = "client-user"
    download_timeout_secs = timedelta(minutes=4).total_seconds()

    def __init__(self, host: str = None, cache_size: int = 0, cache_dir: str = None,
                 client_credentials_scope: str = None, client_credentials_user_name: str = None,
                 client_id: str = None, client_secret: str = None, redirect_port: int = None,
                 scope: str = None, auth_domain: str = None):
        """
        Initialize instance.

        :param host: URL of host - web service
        :param cache_size: size of cache. We try to cache recent results and use them instead of asking from server.
        Use 0 (by default) to disable caching
        """
        self.host = host or self.default_host
        self._cached_results = CachedResults(max_cache_size=cache_size)
        self.cache_dir = cache_dir or self.default_cache_dir
        self.mci_build_dir = None
        self.client_credentials_scope = client_credentials_scope or self.default_client_credentials_scope
        self.client_credentials_user_name = client_credentials_user_name or self.default_client_credentials_user_name
        self.client_id = client_id
        self.client_secret = client_secret
        self.redirect_port = redirect_port
        self.scope = scope
        self.auth_domain = auth_domain
        self.configs = Configs(client_credentials_scope=self.client_credentials_scope,
                               client_credentials_user_name=self.client_credentials_user_name,
                               client_id=self.client_id, auth_domain=self.auth_domain,
                               redirect_port=self.redirect_port, scope=self.scope)
        self.http_client = requests.Session()
        self.path_options = PathOptions()

        # create cache dir if it doesn't exist
        if not os.path.exists(self.cache_dir):
            os.makedirs(self.cache_dir)

        self.authenticate()

    def authenticate(self):
        """Login & get credentials for further requests to web service."""

        # try to read from file
        if os.path.exists(self.default_creds_file_path):
            with open(self.default_creds_file_path) as cfile:
                data = json.loads(cfile.read())
                access_token, expire_time = data.get("access_token"), data.get("expire_time")
                if time.time() < expire_time:
                    # credentials not expired yet
                    self.http_client.headers.update({"Authorization": f"Bearer {access_token}"})
                    return

        if self.client_id and self.client_secret:
            # auth using secrets
            credentials = get_client_cred_oauth_credentials(self.client_id, self.client_secret,
                                                            self.configs)
        else:
            # since we don't have access to secrets, ask user to auth manually
            credentials = get_oauth_credentials(configs=self.configs, print_auth_url=True)
        self.http_client.headers.update({"Authorization": f"Bearer {credentials.access_token}"})

        # write credentials to local file for further useage
        with open(self.default_creds_file_path, "w") as cfile:
            cfile.write(
                json.dumps({
                    "access_token": credentials.access_token,
                    "expire_time": time.time() + credentials.expires_in
                }))

    @staticmethod
    def is_valid_path(path: str) -> bool:
        """
        Sometimes the given path may not be valid: e.g: path for a non-existing file.

        If we need to do extra checks on path, we'll do all of them here.
        :param path: path string
        :return: bool indicating the validation status
        """
        return os.path.exists(path)

    def get_from_cache(self, key: str) -> str or None:
        """
        Try to get value from cache.

        :param key: key string
        :return: value or None (if doesn't exist)
        """
        return self._cached_results.get(key)

    def add_to_cache(self, key: str, value: str) -> Dict[str, str]:
        """
        Add new value to cache.

        :param key: key string
        :param value: value string
        :return: added data as dict
        """
        return self._cached_results.insert(key, value)

    @staticmethod
    def url_to_filename(url: str) -> str:
        """
        Convert URL to local filename.

        :param url: download URL
        :return: full name for local file
        """
        return url.split("/")[-1]

    @staticmethod
    def unpack(path: str) -> str:
        """
        Use to utar/unzip files.

        :param path: full path of file
        :return: full path to directory of unpacked file
        """
        out_dir = path.replace(".tgz", "", 1)
        if not os.path.exists(out_dir):
            os.mkdir(out_dir)

        args = ["tar", "xopf", path, "-C", out_dir, "--strip-components 1"]
        cmd = " ".join(args)
        subprocess.check_call(cmd, shell=True)

        return out_dir

    def download(self, url: str) -> (str, bool):
        """
        Use to download file from URL.

        :param url: URL string
        :return: full path of downloaded file in local filesystem, bool indicating if file is already downloaded or not
        """
        exists_locally = False
        filename = self.url_to_filename(url)
        path = os.path.join(self.cache_dir, filename)
        if not os.path.exists(path):
            print("Downloading the file...")
            self.get_file_from_service(url, path)
        else:
            print("File already exists in cache")
            exists_locally = True
        return path, exists_locally

    def get_file_from_service(self, url: str, local_path: str) -> None:
        """
        Get file from URL and write to a local file.

        :param url: URL string
        :param local_path: full name for local file
        """
        with requests.get(url, stream=True, timeout=self.download_timeout_secs) as response:
            with open(local_path, "wb") as file:
                for chunk in response.iter_content(chunk_size=2 * 1024 * 1024):
                    file.write(chunk)

    def get_dbg_file(self, soinfo: dict) -> str or None:
        """
        To get path for given buildId.

        :param soinfo: soinfo as dict
        :return: path as string or None (if path not found)
        """
        build_id = soinfo.get("buildId", "").lower()
        version = soinfo.get("version")
        binary_name = "mongo"
        # search from cached results
        path = self.get_from_cache(build_id)
        if not path:
            # path does not exist in cache, so we send request to server
            try:
                search_parameters = {"build_id": build_id}
                if version:
                    search_parameters["version"] = version
                print(f"Getting data from service... Search parameters: {search_parameters}")
                response = self.http_client.get(f"{self.host}/find_by_id", params=search_parameters)
                if response.status_code != 200:
                    sys.stderr.write(
                        f"Server returned unsuccessful status: {response.status_code}, "
                        f"response body: {response.text}\n")
                    return None
                else:
                    data = response.json().get("data", {})
                    path, binary_name = data.get("debug_symbols_url"), data.get("file_name")
            except Exception as err:  # noqa pylint: disable=broad-except
                sys.stderr.write(f"Error occurred while trying to get response from server "
                                 f"for buildId({build_id}): {err}\n")
                return None

            # update cached results
            if path:
                self.add_to_cache(build_id, path)

        if not path:
            return None

        # download & unpack debug symbols file and assign `path` to unpacked file's local path
        try:
            dl_path, exists_locally = self.download(path)
            if exists_locally:
                path = dl_path.replace(".tgz", "", 1)
            else:
                print("Downloaded, now unpacking...")
                path = self.unpack(dl_path)
        except Exception as err:  # noqa pylint: disable=broad-except
            sys.stderr.write(f"Failed to download & unpack file: {err}\n")
        # we may have '<name>.debug', '<name>.so' or just executable binary file which may not have file 'extension'.
        # if file has extension, it is good. if not, we should append .debug, because those without extension are
        # from release builds, and their debug symbol files contain .debug extension.
        # we need to map those 2 different file names ('<name>' becomes '<name>.debug').
        if not binary_name.endswith(".debug"):
            binary_name = f"{binary_name}.debug"

        inner_folder_name = self.path_options.get_binary_folder_name(binary_name)

        return os.path.join(path, inner_folder_name, binary_name)


def parse_input(trace_doc, dbg_path_resolver):
    """Return a list of frame dicts from an object of {backtrace: list(), processInfo: dict()}."""

    def make_base_addr_map(somap_list):
        """Return map from binary load address to description of library from the somap_list.

        The somap_list is a list of dictionaries describing individual loaded libraries.
        """
        return {so_entry["b"]: so_entry for so_entry in somap_list if "b" in so_entry}

    base_addr_map = make_base_addr_map(trace_doc["processInfo"]["somap"])
    version = get_version(trace_doc)

    frames = []
    for frame in trace_doc["backtrace"]:
        if "b" not in frame:
            print(
                f"Ignoring frame {frame} as it's missing the `b` field; See SERVER-58863 for discussions"
            )
            continue
        soinfo = base_addr_map.get(frame["b"], {})
        if version:
            soinfo["version"] = version
        elf_type = soinfo.get("elfType", 0)
        if elf_type == 3:
            addr_base = "0"
        elif elf_type == 2:
            addr_base = frame["b"]
        else:
            addr_base = soinfo.get("vmaddr", "0")
        addr = int(addr_base, 16) + int(frame["o"], 16)
        # addr currently points to the return address which is the one *after* the call. x86 is
        # variable length so going backwards is difficult. However, llvm-symbolizer seems to do the
        # right thing if we just subtract 1 byte here. This has the downside of also adjusting the
        # address of instructions that cause signals (such as segfaults and divide-by-zero) which
        # are already correct, but there doesn't seem to be a reliable way to detect that case.
        addr -= 1
        frames.append(
            dict(
                path=dbg_path_resolver.get_dbg_file(soinfo), buildId=soinfo.get("buildId", None),
                offset=frame["o"], addr="0x{:x}".format(addr), symbol=frame.get("s", None)))
    return frames


def get_version(trace_doc: Dict[str, Any]) -> Optional[str]:
    """
    Get version from trace doc.

    :param trace_doc: Traceback dict.
    :return: Version string or None.
    """
    return trace_doc.get("processInfo", {}).get("mongodbVersion")


def symbolize_frames(trace_doc, dbg_path_resolver, symbolizer_path, dsym_hint, input_format,
                     **kwargs):
    """Return a list of symbolized stack frames from a trace_doc in MongoDB stack dump format."""

    # Keep frames in kwargs to avoid changing the function signature.
    frames = kwargs.get("frames")
    if frames is None:
        total_seconds_for_retries = kwargs.get("total_seconds_for_retries", 0)
        frames = preprocess_frames_with_retries(dbg_path_resolver, trace_doc, input_format,
                                                total_seconds_for_retries)

    if not symbolizer_path:
        symbolizer_path = os.environ.get(SYMBOLIZER_PATH_ENV)
        if not symbolizer_path:
            print(f"Env value for '{SYMBOLIZER_PATH_ENV}' not found, using"
                  f" '{DEFAULT_SYMBOLIZER_PATH}' as a default executable path.")
            symbolizer_path = DEFAULT_SYMBOLIZER_PATH

    symbolizer_args = [symbolizer_path]
    for dh in dsym_hint:
        symbolizer_args.append("-dsym-hint={}".format(dh))
    symbolizer_process = subprocess.Popen(args=symbolizer_args, close_fds=True,
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=sys.stdout)

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
            print(f"Path not found in frame: {frame}")
            continue
        symbol_line = "CODE {path:} {addr:}\n".format(**frame)
        symbolizer_process.stdin.write(symbol_line.encode())
        symbolizer_process.stdin.flush()
        frame["symbinfo"] = extract_symbols(symbolizer_process.stdout)
    symbolizer_process.stdin.close()
    symbolizer_process.wait()
    return frames


def preprocess_frames(dbg_path_resolver: DbgFileResolver, trace_doc: Dict[str, Any],
                      input_format: str) -> List[Dict[str, Any]]:
    """
    Process the paths in frame objects.

    :param dbg_path_resolver: debug symbols file path resolver
    :param trace_doc: traceback object
    :param input_format: format of input
    :return: the list of traceback frames
    """

    if input_format == "classic":
        frames = parse_input(trace_doc, dbg_path_resolver)
    elif input_format == "thin":
        frames = trace_doc["backtrace"]
        for frame in frames:
            frame["path"] = dbg_path_resolver.get_dbg_file(frame)
    else:
        raise ValueError('Unknown input format "{}"'.format(input_format))

    return frames


def has_high_not_found_paths_ratio(frames: List[Dict[str, Any]]) -> bool:
    """
    Check whether not found paths in frames ratio is higher than 0.5.

    :param frames: the list of traceback frames
    :return: True if ratio is higher than 0.5
    """
    not_found = [1 for f in frames if f.get("path") is None]
    not_found_ratio = len(not_found) / (len(frames) or 1)
    return not_found_ratio >= 0.5


def preprocess_frames_with_retries(dbg_path_resolver: DbgFileResolver, trace_doc: Dict[str, Any],
                                   input_format: str,
                                   total_seconds_for_retries: int = 0) -> List[Dict[str, Any]]:
    """
    Process the paths in frame objects.

    :param dbg_path_resolver: debug symbols file path resolver
    :param trace_doc: traceback object
    :param input_format: format of input
    :param total_seconds_for_retries: max wait time for retries in seconds
    :return: the list of traceback frames
    """

    retrying = Retrying(
        retry=retry_if_result(has_high_not_found_paths_ratio), wait=wait_fixed(60),
        stop=stop_after_delay(total_seconds_for_retries),
        retry_error_callback=lambda retry_state: retry_state.outcome.result())

    return retrying(preprocess_frames, dbg_path_resolver, trace_doc, input_format)


def classic_output(frames, outfile, **kwargs):  # pylint: disable=unused-argument
    """Provide classic output."""
    for frame in frames:
        symbinfo = frame.get("symbinfo")
        if symbinfo:
            for sframe in symbinfo:
                outfile.write(" {file:s}:{line:d}:{column:d}: {fn:s}\n".format(**sframe))
        else:
            outfile.write(" Couldn't extract symbols: path={path}\n".format(
                path=frame.get('path', 'no value found')))


def make_argument_parser(parser=None, **kwargs):
    """Make and return an argparse."""
    if parser is None:
        parser = argparse.ArgumentParser(**kwargs)

    parser.add_argument('--dsym-hint', default=[], action='append')
    parser.add_argument('--symbolizer-path', default='')
    parser.add_argument('--input-format', choices=['classic', 'thin'], default='classic')
    parser.add_argument('--output-format', choices=['classic', 'json'], default='classic',
                        help='"json" shows some extra information')
    parser.add_argument('--debug-file-resolver', choices=['path', 's3', 'pr'], default='pr')
    parser.add_argument('--src-dir-to-move', action="store", type=str, default=None,
                        help="Specify a src dir to move to /data/mci/{original_buildid}/src")
    parser.add_argument(
        '--total-seconds-for-retries', default=0, type=int,
        help="If web service fails to find path for given build id, it could be because mapping "
        "process was not finished yet. We can wait for it to finish and retry again. Each retry"
        " adds 2 minutes to previous wait time. It is guaranteed that total wait time does not exceed this "
        "specified amount.")

    parser.add_argument('--live', action='store_true')
    s3_group = parser.add_argument_group(
        "s3 options", description='Options used with \'--debug-file-resolver s3\'')
    s3_group.add_argument('--s3-cache-dir')
    s3_group.add_argument('--s3-bucket')

    pr_group = parser.add_argument_group(
        'Path Resolver options (Path Resolver uses a special web service to retrieve URL of debug symbols file for '
        'a given BuildID), we use "pr" as a shorter/easier name for this',
        description='Options used with \'--debug-file-resolver pr\'')
    pr_group.add_argument('--pr-host', default='',
                          help='URL of web service running the API to get debug symbol URL')
    pr_group.add_argument('--pr-cache-dir', default='',
                          help='Full path to a directory to store cache/files')
    pr_group.add_argument('--client-secret', default='', help='Secret key for Okta Oauth')
    pr_group.add_argument('--client-id', default='', help='Client id for Okta Oauth')
    # caching mechanism is currently not fully developed and needs more advanced cleaning techniques, we add an option
    # to enable it after completing the implementation

    # Look for symbols in the cwd by default.
    parser.add_argument('path_to_executable', nargs="?")
    return parser


def substitute_stdin(options, resolver):
    """Accept stdin stream as source of logs and symbolize it."""

    # Ignore Ctrl-C. When the process feeding the pipe exits, `stdin` will be closed.
    signal.signal(signal.SIGINT, signal.SIG_IGN)

    print("Live mode activated, waiting for input...")
    while True:
        backtrace_indicator = '{"backtrace":'
        line = sys.stdin.readline()
        if not line:
            return

        line = line.strip()

        if 'Frame: 0x' in line:
            continue

        if backtrace_indicator in line:
            backtrace_index = line.index(backtrace_indicator)
            prefix = line[:backtrace_index]
            backtrace = line[backtrace_index:]
            trace_doc = json.loads(backtrace)
            if not trace_doc["backtrace"]:
                print("Trace is empty, skipping...")
                continue
            frames = symbolize_frames(
                trace_doc,
                resolver,
                options.symbolizer_path,
                [],
                options.output_format,
            )
            print(prefix)
            print("Symbolizing...")
            classic_output(frames, sys.stdout, indent=2)
            print("Completed, waiting for input...")
        else:
            print(line)


def main(options):
    """Execute Main program."""

    resolver = None
    if options.debug_file_resolver == 'path':
        resolver = PathDbgFileResolver(options.path_to_executable)
    elif options.debug_file_resolver == 's3':
        resolver = S3BuildidDbgFileResolver(options.s3_cache_dir, options.s3_bucket)
    elif options.debug_file_resolver == 'pr':
        resolver = PathResolver(host=options.pr_host, cache_dir=options.pr_cache_dir,
                                client_secret=options.client_secret, client_id=options.client_id)

    if options.live:
        print("Entering live mode")
        substitute_stdin(options, resolver)
        sys.exit(0)

    # Skip over everything before the first '{' since it is likely to be log line prefixes.
    # Additionally, using raw_decode() to ignore extra data after the closing '}' to allow maximal
    # sloppiness in copy-pasting input.
    trace_doc = sys.stdin.read()

    if not trace_doc or not trace_doc.strip():
        print("Please provide the backtrace through stdin for symbolization;"
              " e.g. `your/symbolization/command < /file/with/stacktrace`")

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

    # given a log file including traceback,
    # we try to find traceback from that file, analyzing each line until we find it
    for line in trace_doc.splitlines():
        possible_trace_doc = line[line.find('{'):]
        try:
            possible_trace_doc = json.JSONDecoder().raw_decode(possible_trace_doc)[0]
            trace_doc = bt_search(possible_trace_doc)
            if trace_doc:
                break
        except json.JSONDecodeError:
            pass
    else:
        sys.stderr.write("could not find json backtrace object in input\n")
        sys.exit(1)

    output_fn = None
    if options.output_format == 'json':
        output_fn = json.dump
    if options.output_format == 'classic':
        output_fn = classic_output

    frames = preprocess_frames_with_retries(resolver, trace_doc, options.input_format,
                                            options.total_seconds_for_retries)

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
