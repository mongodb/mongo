#!/usr/bin/env python3
#
# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""
This script is intended to be called by ninja to start up the scons daemon process. It will
launch the server and attempt to connect to it. This process needs to completely detach
from the spawned process so ninja can consider the build edge completed. It should be passed
the args which should be forwarded to the scons daemon process which could be any number of
# arguments. However the first few arguments are required to be port, ninja dir, and keep alive
timeout in seconds.

The scons_daemon_dirty file acts as a pidfile marker letting this script quickly skip over
restarting the server if the server is running. The assumption here is the pidfile should only
exist if the server is running.
"""

import subprocess
import sys
import os
import pathlib
import tempfile
import hashlib
import logging
import time
import http.client
import traceback
import socket

ninja_builddir = pathlib.Path(sys.argv[2])
daemon_dir = pathlib.Path(tempfile.gettempdir()) / (
    "scons_daemon_" + str(hashlib.md5(str(ninja_builddir).encode()).hexdigest())
)
os.makedirs(daemon_dir, exist_ok=True)

logging.basicConfig(
    filename=daemon_dir / "scons_daemon.log",
    filemode="a",
    format="%(asctime)s %(message)s",
    level=logging.DEBUG,
)

def log_error(msg) -> None:
    logging.debug(msg)
    sys.stderr.write(msg)

if not os.path.exists(ninja_builddir / "scons_daemon_dirty"):
    cmd = [
        sys.executable,
        str(pathlib.Path(__file__).parent / "ninja_scons_daemon.py"),
    ] + sys.argv[1:]
    logging.debug(f"Starting daemon with {' '.join(cmd)}")

    
    # TODO: Remove the following when Python3.6 support is dropped.
    if sys.platform == 'win32' and sys.version_info[0] == 3 and sys.version_info[1] == 6:
        # on Windows with Python version 3.6, popen does not do a good job disconnecting
        # the std handles and this make ninja hang because they stay open to the original
        # process ninja launched. Here we can force the handles to be separated.
        # See: https://docs.python.org/3.6/library/subprocess.html#subprocess.STARTUPINFO
        # See Also: https://docs.python.org/3.6/library/subprocess.html#subprocess.Popen
        # Note when you don't specify stdin, stdout, and/or stderr they default to None
        # which indicates no output redirection will occur.
        si = subprocess.STARTUPINFO()
        si.dwFlags = subprocess.STARTF_USESTDHANDLES
        p = subprocess.Popen(
            cmd, close_fds=True, shell=False, startupinfo=si
        )
    else:
        p = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, shell=False,
        )
    with open(daemon_dir / "pidfile", "w") as f:
        f.write(str(p.pid))
    with open(ninja_builddir / "scons_daemon_dirty", "w") as f:
        f.write(str(p.pid))

    error_msg = f"ERROR: Failed to connect to scons daemon.\n Check {daemon_dir / 'scons_daemon.log'} for more info.\n"

    while True:
        try:
            logging.debug("Attempting to connect scons daemon")
            conn = http.client.HTTPConnection(
                "127.0.0.1", port=int(sys.argv[1]), timeout=60
            )
            conn.request("GET", "/?ready=true")
            response = None

            try:
                response = conn.getresponse()
            except (http.client.RemoteDisconnected, http.client.ResponseNotReady, socket.timeout):
                time.sleep(0.01)
            except http.client.HTTPException:
                log_error(f"Error: {traceback.format_exc()}")
                exit(1)
            else:
                msg = response.read()
                status = response.status
                if status != 200:
                    log_error(msg.decode("utf-8"))
                    exit(1)
                logging.debug("Server Responded it was ready!")
                break

        except ConnectionRefusedError:
            logging.debug(f"Server not ready, server PID: {p.pid}")
            time.sleep(1)
            if p.poll() is not None:
                log_error(f"Server process died, aborting: {p.returncode}")
                sys.exit(p.returncode)
        except ConnectionResetError:
            log_error("Server ConnectionResetError")
            exit(1)
        except Exception:
            log_error(f"Error: {traceback.format_exc()}")
            exit(1)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
