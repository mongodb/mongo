import sys
import random
import string
import logging
import os
import re
import shutil
from subprocess import Popen, PIPE

RNP_ROOT = None
WORKDIR = ''
CONSOLE_ENCODING = 'UTF-8'

class CLIError(Exception):
    def __init__(self, message, log = None):
        super(CLIError, self).__init__(message)
        self.message = message
        self.log = log
        logging.info(self.message)
        logging.debug(self.log.strip())

    def __str__(self):
        return self.message + '\n' + self.log

def set_workdir(dir):
    global WORKDIR
    WORKDIR = dir

def is_windows():
    return sys.platform.startswith('win') or sys.platform.startswith('msys')

def path_for_gpg(path):
    # GPG built for mingw/msys doesn't work with Windows paths
    if re.match(r'^[a-z]:[\\\/].*', path.lower()):
        path = '/' + path[0] + '/' + path[3:].replace('\\', '/')
    return path

def raise_err(msg, log = None):
    raise CLIError(msg, log)

def size_to_readable(num, suffix = 'B'):
    for unit in ['', 'K', 'M', 'G', 'T', 'P', 'E', 'Z']:
        if abs(num) < 1024.0:
            return "%3.1f%s%s" % (num, unit, suffix)
        num /= 1024.0
    return "%.1f%s%s" % (num, 'Yi', suffix)

def list_upto(lst, count):
    return (list(lst)*(count//len(lst)+1))[:count]

def pswd_pipe(password):
    pr, pw = os.pipe()
    with os.fdopen(pw, 'w') as fw:
        fw.write(password)
        fw.write('\n')
        fw.write(password)
    os.set_inheritable(pr, True)

    if not is_windows():
        return pr
    # On Windows pipe is not inheritable so dup() is needed
    prd = os.dup(pr)
    os.close(pr)
    return prd

def random_text(path, size):
    # Generate random text, with 50% probability good-compressible
    if random.randint(0, 10) < 5:
        st = ''.join(random.choice(string.ascii_letters + string.digits + " \t\r\n-,.")
                     for _ in range(size))
    else:
        st = ''.join(random.choice("abcdef0123456789 \t\r\n-,.") for _ in range(size))
    with open(path, 'w+') as f:
        f.write(st)

def file_text(path, encoding = CONSOLE_ENCODING):
    with open(path, 'rb') as f:
        return f.read().decode(encoding).replace('\r\r', '\r')

def find_utility(name, exitifnone = True):
    path = shutil.which(name)
    if not path and exitifnone:
        logging.error('Cannot find utility {}. Exiting.'.format(name))
        sys.exit(1)

    return path

def rnp_file_path(relpath, check = True):
    global RNP_ROOT
    if not RNP_ROOT:
        pypath = os.path.dirname(__file__)
        RNP_ROOT = os.path.realpath(os.path.join(pypath, '../..'))

    fpath = os.path.realpath(os.path.join(RNP_ROOT, relpath))

    if check and not os.path.isfile(fpath):
        raise NameError('rnp: file ' + relpath + ' not found')

    return fpath

def run_proc_windows(proc, params, stdin=None):
    exe = os.path.basename(proc)
    # test special quote cases 
    params = list(map(lambda st: st.replace('"', '\\"'), params))
    # We need to escape empty parameters/ones with spaces with quotes
    params = tuple(map(lambda st: st if (st and not any(x in st for x in [' ','\r','\t'])) else '"%s"' % st, [exe] + params))
    logging.debug((proc + ' ' + ' '.join(params)).strip())
    logging.debug('Working directory: ' + os.getcwd())
    sys.stdout.flush()

    stdin_path = os.path.join(WORKDIR, 'stdin.txt')
    stdout_path = os.path.join(WORKDIR, 'stdout.txt')
    stderr_path = os.path.join(WORKDIR, 'stderr.txt')
    pass_path = os.path.join(WORKDIR, 'pass.txt')
    passfd = 0
    passfo = None
    try:
        idx = params.index('--pass-fd')
        if idx < len(params):
            passfd = int(params[idx+1])
            passfo = os.fdopen(passfd, 'r', closefd=False)
    except (ValueError, OSError):
        # Ignore if pass-fd is invalid/could not be opened
        pass
    # We may use pipes here (ensuring we use dup to inherit handles), but those have limited buffer
    # so we'll need to poll process
    if stdin:
        with open(stdin_path, "wb+") as stdinf:
            stdinf.write(stdin.encode() if isinstance(stdin, str) else stdin)
        stdin_fl = os.open(stdin_path, os.O_RDONLY | os.O_BINARY)
        stdin_no = sys.stdin.fileno()
        stdin_cp = os.dup(stdin_no)
    else:
        stdin_fl = None
        stdin_no = -1
        stdin_cp = None

    stdout_fl = os.open(stdout_path, os.O_CREAT | os.O_RDWR | os.O_BINARY)
    stdout_no = sys.stdout.fileno()
    stdout_cp = os.dup(stdout_no)
    stderr_fl = os.open(stderr_path, os.O_CREAT | os.O_RDWR | os.O_BINARY)
    stderr_no = sys.stderr.fileno()
    stderr_cp = os.dup(stderr_no)
    if passfo:
        with open(pass_path, "w+") as passf:
            passf.write(passfo.read())
        pass_fl = os.open(pass_path, os.O_RDONLY | os.O_BINARY)
        pass_cp = os.dup(passfd)

    retcode = -1
    try:
        os.dup2(stdout_fl, stdout_no)
        os.close(stdout_fl)
        os.dup2(stderr_fl, stderr_no)
        os.close(stderr_fl)
        if stdin:
            os.dup2(stdin_fl, stdin_no)
            os.close(stdin_fl)
        if passfo:
            os.dup2(pass_fl, passfd)
            os.close(pass_fl)
        retcode = os.spawnv(os.P_WAIT, proc, params)
    finally:
        os.dup2(stdout_cp, stdout_no)
        os.close(stdout_cp)
        os.dup2(stderr_cp, stderr_no)
        os.close(stderr_cp)
        if stdin:
            os.dup2(stdin_cp, stdin_no)
            os.close(stdin_cp)
        if passfo:
            os.dup2(pass_cp, passfd)
            os.close(pass_cp)
            passfo.close()
    out = file_text(stdout_path).replace('\r\n', '\n')
    err = file_text(stderr_path).replace('\r\n', '\n')
    os.unlink(stdout_path)
    os.unlink(stderr_path)
    if stdin: 
        os.unlink(stdin_path)
    if passfo: 
        os.unlink(pass_path)
    logging.debug(err.strip())
    logging.debug(out.strip())
    return (retcode, out, err)

if sys.version_info >= (3,):
    def decode_string_escape(s):
        bts = bytes(s, 'utf-8')
        result = u''
        candidate = bytearray()
        utf = bytearray()
        for b in bts:
            if b > 0x7F:
                if len(candidate) > 0:
                    result += candidate.decode('unicode-escape')
                    candidate.clear()
                utf.append(b)
            else:
                if len(utf) > 0:
                    result += utf.decode('utf-8')
                    utf.clear()
                candidate.append(b)
        if len(candidate) > 0:
            result += candidate.decode('unicode-escape')
        if len(utf) > 0:
            result += utf.decode('utf-8')
        return result
    def _decode(s):
        return s
else: # Python 2
    def decode_string_escape(s):
        return s.encode(CONSOLE_ENCODING).decode('decode_string_escape')
    def _decode(x):
        return x.decode(CONSOLE_ENCODING)

def run_proc(proc, params, stdin=None):
    # On Windows we need to use spawnv() for handle inheritance in pswd_pipe()
    if is_windows():
        return run_proc_windows(proc, params, stdin)
    paramline = u' '.join(map(_decode, params))
    logging.debug((proc + ' ' + paramline).strip())
    param_bytes = list(map(lambda x: x.encode(CONSOLE_ENCODING), params))
    process = Popen([proc] + param_bytes, stdout=PIPE, stderr=PIPE,
                    stdin=PIPE if stdin else None, close_fds=False,
                    universal_newlines=True)
    output, errout = process.communicate(stdin)
    retcode = process.poll()
    logging.debug(errout.strip())
    logging.debug(output.strip())

    return (retcode, output, errout)

def run_proc_fast(proc, params):
    with open(os.devnull, 'w') as devnull:
        proc = Popen([proc] + params, stdout=devnull, stderr=devnull)
    return proc.wait()
