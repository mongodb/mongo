"""
buildlogger.py

Wrap a command (specified on the command line invocation of buildlogger.py)
and send output in batches to the buildlogs web application via HTTP POST.

The script configures itself from environment variables:

  required env vars:
    MONGO_BUILDER_NAME (e.g. "Nightly Linux 64-bit")
    MONGO_BUILD_NUMBER (an integer)
    MONGO_TEST_FILENAME (not required when invoked with -g)

  optional env vars:
    MONGO_PHASE (e.g. "core", "slow nightly", etc)
    MONGO_* (any other environment vars are passed to the web app)
    BUILDLOGGER_CREDENTIALS (see below)

This script has two modes: a "test" mode, intended to wrap the invocation of
an individual test file, and a "global" mode, intended to wrap the mongod
instances that run throughout the duration of a mongo test phase (the logs
from "global" invocations are displayed interspersed with the logs of each
test, in order to let the buildlogs web app display the full output sensibly.)

If the BUILDLOGGER_CREDENTIALS environment variable is set, it should be a
path to a valid Python file containing "username" and "password" variables,
which should be valid credentials for authenticating to the buildlogger web
app. For example:

    username = "hello"
    password = "world"

If BUILDLOGGER_CREDENTIALS is a relative path, then the working directory
and the directories one, two, and three levels up, are searched, in that
order.
"""

import functools
import os
import os.path
import re
import signal
import socket
import subprocess
import sys
import time
import traceback
import urllib2
import utils

# suppress deprecation warnings that happen when
# we import the 'buildbot.tac' file below
import warnings
warnings.simplefilter('ignore', DeprecationWarning)

try:
    import json
except:
    try:
        import simplejson as json
    except:
        json = None

# try to load the shared secret from settings.py
# which will be one, two, or three directories up
# from this file's location
credentials_file = os.environ.get('BUILDLOGGER_CREDENTIALS', 'buildbot.tac')
credentials_loc, credentials_name = os.path.split(credentials_file)
if not credentials_loc:
    here = os.path.abspath(os.path.dirname(__file__))
    possible_paths = [
        os.path.abspath(os.path.join(here, '..')),
        os.path.abspath(os.path.join(here, '..', '..')),
        os.path.abspath(os.path.join(here, '..', '..', '..')),
    ]
else:
    possible_paths = [credentials_loc]

username, password = None, None
for path in possible_paths:
    credentials_path = os.path.join(path, credentials_name)
    if os.path.isfile(credentials_path):
        credentials = {}
        try:
            execfile(credentials_path, credentials, credentials)
            username = credentials.get('slavename', credentials.get('username'))
            password = credentials.get('passwd', credentials.get('password'))
            break
        except:
            pass


URL_ROOT = os.environ.get('BUILDLOGGER_URL', 'http://buildlogs.mongodb.org/')
TIMEOUT_SECONDS = 10
socket.setdefaulttimeout(TIMEOUT_SECONDS)

digest_handler = urllib2.HTTPDigestAuthHandler()
digest_handler.add_password(
    realm='buildlogs',
    uri=URL_ROOT,
    user=username,
    passwd=password)

# This version of HTTPErrorProcessor is copied from
# Python 2.7, and allows REST response codes (e.g.
# "201 Created") which are treated as errors by
# older versions.
class HTTPErrorProcessor(urllib2.HTTPErrorProcessor):
    def http_response(self, request, response):
        code, msg, hdrs = response.code, response.msg, response.info()

        # According to RFC 2616, "2xx" code indicates that the client's
        # request was successfully received, understood, and accepted.
        if not (200 <= code < 300):
            response = self.parent.error(
                'http', request, response, code, msg, hdrs)

        return response

url_opener = urllib2.build_opener(digest_handler, HTTPErrorProcessor())

def url(endpoint):
    if not endpoint.endswith('/'):
        endpoint = '%s/' % endpoint

    return '%s/%s' % (URL_ROOT.rstrip('/'), endpoint)

def post(endpoint, data, headers=None):
    data = json.dumps(data, encoding='utf-8')

    headers = headers or {}
    headers.update({'Content-Type': 'application/json; charset=utf-8'})

    req = urllib2.Request(url=url(endpoint), data=data, headers=headers)
    try:
        response = url_opener.open(req)
    except urllib2.URLError:
        import traceback
        traceback.print_exc(file=sys.stderr)
        sys.stderr.flush()
        # indicate that the request did not succeed
        return None

    response_headers = dict(response.info())

    # eg "Content-Type: application/json; charset=utf-8"
    content_type = response_headers.get('content-type')
    match = re.match(r'(?P<mimetype>[^;]+).*(?:charset=(?P<charset>[^ ]+))?$', content_type)
    if match and match.group('mimetype') == 'application/json':
        encoding = match.group('charset') or 'utf-8'
        return json.load(response, encoding=encoding)

    return response.read()

def traceback_to_stderr(func):
    """
    decorator which logs any exceptions encountered to stderr
    and returns none.
    """
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        try:
            return func(*args, **kwargs)
        except urllib2.HTTPError, err:
            sys.stderr.write('error: HTTP code %d\n----\n' % err.code)
            if hasattr(err, 'hdrs'):
                for k, v in err.hdrs.items():
                    sys.stderr.write("%s: %s\n" % (k, v))
                sys.stderr.write('\n')
            sys.stderr.write(err.read())
            sys.stderr.write('\n----\n')
            sys.stderr.flush()
        except:
            sys.stderr.write('Traceback from buildlogger:\n')
            traceback.print_exc(file=sys.stderr)
            sys.stderr.flush()
        return None
    return wrapper


@traceback_to_stderr
def get_or_create_build(builder, buildnum, extra={}):
    data = {'builder': builder, 'buildnum': buildnum}
    data.update(extra)
    response = post('build', data)
    if response is None:
        return None
    return response['id']

@traceback_to_stderr
def create_test(build_id, test_filename, test_command, test_phase):
    response = post('build/%s/test' % build_id, {
        'test_filename': test_filename,
        'command': test_command,
        'phase': test_phase,
    })
    if response is None:
        return None
    return response['id']

@traceback_to_stderr
def append_test_logs(build_id, test_id, log_lines):
    response = post('build/%s/test/%s' % (build_id, test_id), data=log_lines)
    if response is None:
        return False
    return True

@traceback_to_stderr
def append_global_logs(build_id, log_lines):
    """
    "global" logs are for the mongod(s) started by smoke.py
    that last the duration of a test phase -- since there
    may be output in here that is important but spans individual
    tests, the buildlogs webapp handles these logs specially.
    """
    response = post('build/%s' % build_id, data=log_lines)
    if response is None:
        return False
    return True

@traceback_to_stderr
def finish_test(build_id, test_id, failed=False):
    response = post('build/%s/test/%s' % (build_id, test_id), data=[], headers={
        'X-Sendlogs-Test-Done': 'true',
        'X-Sendlogs-Test-Failed': failed and 'true' or 'false',
    })
    if response is None:
        return False
    return True

def run_and_echo(command):
    """
    this just calls the command, and returns its return code,
    allowing stdout and stderr to work as normal. it is used
    as a fallback when environment variables or python
    dependencies cannot be configured, or when the logging
    webapp is unavailable, etc
    """
    proc = subprocess.Popen(command)

    def handle_sigterm(signum, frame):
        try:
            proc.send_signal(signum)
        except AttributeError:
            os.kill(proc.pid, signum)
    orig_handler = signal.signal(signal.SIGTERM, handle_sigterm)

    proc.wait()

    signal.signal(signal.SIGTERM, orig_handler)
    return proc.returncode

class LogAppender(object):
    def __init__(self, callback, args, send_after_lines=2000, send_after_seconds=10):
        self.callback = callback
        self.callback_args = args

        self.send_after_lines = send_after_lines
        self.send_after_seconds = send_after_seconds

        self.buf = []
        self.retrybuf = []
        self.last_sent = time.time()

    def __call__(self, line):
        self.buf.append((time.time(), line))

        delay = time.time() - self.last_sent
        if len(self.buf) >= self.send_after_lines or delay >= self.send_after_seconds:
            self.submit()

        # no return value is expected

    def submit(self):
        if len(self.buf) + len(self.retrybuf) == 0:
            return True

        args = list(self.callback_args)
        args.append(list(self.buf) + self.retrybuf)

        self.last_sent = time.time()

        if self.callback(*args):
            self.buf = []
            self.retrybuf = []
            return True
        else:
            self.retrybuf += self.buf
            self.buf = []
            return False


def wrap_test(command):
    """
    call the given command, intercept its stdout and stderr,
    and send results in batches of 100 lines or 10s to the 
    buildlogger webapp
    """

    # get builder name and build number from environment
    builder = os.environ.get('MONGO_BUILDER_NAME')
    buildnum = os.environ.get('MONGO_BUILD_NUMBER')

    if builder is None or buildnum is None:
        return run_and_echo(command)

    try:
        buildnum = int(buildnum)
    except ValueError:
        sys.stderr.write('buildlogger: build number ("%s") was not an int\n' % buildnum)
        sys.stderr.flush()
        return run_and_echo(command)

    # test takes some extra info
    phase = os.environ.get('MONGO_PHASE', 'unknown')
    test_filename = os.environ.get('MONGO_TEST_FILENAME', 'unknown')

    build_info = dict((k, v) for k, v in os.environ.items() if k.startswith('MONGO_'))
    build_info.pop('MONGO_BUILDER_NAME', None)
    build_info.pop('MONGO_BUILD_NUMBER', None)
    build_info.pop('MONGO_PHASE', None)
    build_info.pop('MONGO_TEST_FILENAME', None)

    build_id = get_or_create_build(builder, buildnum, extra=build_info)
    if not build_id:
        return run_and_echo(command)

    test_id = create_test(build_id, test_filename, ' '.join(command), phase)
    if not test_id:
        return run_and_echo(command)

    # the peculiar formatting here matches what is printed by
    # smoke.py when starting tests
    output_url = '%s/build/%s/test/%s/' % (URL_ROOT.rstrip('/'), build_id, test_id)
    sys.stdout.write('                (output suppressed; see %s)\n' % output_url)
    sys.stdout.flush()

    callback = LogAppender(callback=append_test_logs, args=(build_id, test_id))
    returncode = loop_and_callback(command, callback)
    failed = bool(returncode != 0)

    # this will append any remaining unsubmitted logs, or
    # return True if there are none left to submit
    tries = 5
    while not callback.submit() and tries > 0:
        sys.stderr.write('failed to finish sending test logs, retrying in 1s\n')
        sys.stderr.flush()
        time.sleep(1)
        tries -= 1

    tries = 5
    while not finish_test(build_id, test_id, failed) and tries > 5:
        sys.stderr.write('failed to mark test finished, retrying in 1s\n')
        sys.stderr.flush()
        time.sleep(1)
        tries -= 1

    return returncode

def wrap_global(command):
    """
    call the given command, intercept its stdout and stderr,
    and send results in batches of 100 lines or 10s to the
    buildlogger webapp. see :func:`append_global_logs` for the
    difference between "global" and "test" log output.
    """

    # get builder name and build number from environment
    builder = os.environ.get('MONGO_BUILDER_NAME')
    buildnum = os.environ.get('MONGO_BUILD_NUMBER')

    if builder is None or buildnum is None:
        return run_and_echo(command)

    try:
        buildnum = int(buildnum)
    except ValueError:
        sys.stderr.write('int(os.environ["MONGO_BUILD_NUMBER"]):\n')
        sys.stderr.write(traceback.format_exc())
        sys.stderr.flush()
        return run_and_echo(command)

    build_info = dict((k, v) for k, v in os.environ.items() if k.startswith('MONGO_'))
    build_info.pop('MONGO_BUILDER_NAME', None)
    build_info.pop('MONGO_BUILD_NUMBER', None)

    build_id = get_or_create_build(builder, buildnum, extra=build_info)
    if not build_id:
        return run_and_echo(command)

    callback = LogAppender(callback=append_global_logs, args=(build_id, ))
    returncode = loop_and_callback(command, callback)

    # this will append any remaining unsubmitted logs, or
    # return True if there are none left to submit
    tries = 5
    while not callback.submit() and tries > 0:
        sys.stderr.write('failed to finish sending global logs, retrying in 1s\n')
        sys.stderr.flush()
        time.sleep(1)
        tries -= 1

    return returncode

def loop_and_callback(command, callback):
    """
    run the given command (a sequence of arguments, ordinarily
    from sys.argv), and call the given callback with each line
    of stdout or stderr encountered. after the command is finished,
    callback is called once more with None instead of a string.
    """
    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    def handle_sigterm(signum, frame):
        try:
            proc.send_signal(signum)
        except AttributeError:
            os.kill(proc.pid, signum)

    # register a handler to delegate SIGTERM
    # to the child process
    orig_handler = signal.signal(signal.SIGTERM, handle_sigterm)

    while proc.poll() is None:
        try:
            line = proc.stdout.readline().strip('\r\n')
            line = utils.unicode_dammit(line)
            callback(line)
        except IOError:
            # if the signal handler is called while
            # we're waiting for readline() to return,
            # don't show a traceback
            break

    # There may be additional buffered output
    for line in proc.stdout.readlines():
        callback(line.strip('\r\n'))

    # restore the original signal handler, if any
    signal.signal(signal.SIGTERM, orig_handler)
    return proc.returncode


if __name__ == '__main__':
    # argv[0] is 'buildlogger.py'
    del sys.argv[0]

    if sys.argv[0] in ('-g', '--global'):
        # then this is wrapping a "global" command, and should
        # submit global logs to the build, not test logs to a
        # test within the build
        del sys.argv[0]
        wrapper = wrap_global

    else:
        wrapper = wrap_test

    # if we are missing credentials or the json module, then
    # we can't use buildlogger; so just echo output, but also
    # log why we can't work.
    if json is None:
        sys.stderr.write('buildlogger: could not import a json module\n')
        sys.stderr.flush()
        wrapper = run_and_echo

    elif username is None or password is None:
        sys.stderr.write('buildlogger: could not find or import %s for authentication\n' % credentials_file)
        sys.stderr.flush()
        wrapper = run_and_echo

    # otherwise wrap a test command as normal; the
    # wrapper functions return the return code of
    # the wrapped command, so that should be our
    # exit code as well.
    sys.exit(wrapper(sys.argv))

