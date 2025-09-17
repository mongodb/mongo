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
This script primarily consists of two threads, the http server thread and the scons interactive
process thread. The http server thread will listen on the passed port for http get request
which should indicate some action for the scons interactive process to take.

The daemon will keep log files in a tmp directory correlated to the hash of the absolute path
of the ninja build dir passed. The daemon will also use a keep alive time to know when to shut
itself down after the passed timeout of no activity. Any time the server receives a get request,
the keep alive time will be reset.
"""

import http.server
import socketserver
from urllib.parse import urlparse, parse_qs
import time
from threading import Condition
from subprocess import PIPE, Popen
import sys
import os
import threading
import queue
import pathlib
import logging
from timeit import default_timer as timer
import traceback
import tempfile
import hashlib
import signal

port = int(sys.argv[1])
ninja_builddir = pathlib.Path(sys.argv[2])
daemon_keep_alive = int(sys.argv[3])
args = sys.argv[4:]

# TODO: Remove the following when Python3.6 support is dropped.
# Windows and Python36 passed nothing for the std handles because of issues with popen
# and its handles so we have to make some fake ones to prevent exceptions.
if sys.platform == 'win32' and sys.version_info[0] == 3 and sys.version_info[1] == 6:
    from io import StringIO
    sys.stderr = StringIO()
    sys.stdout = StringIO()

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


def daemon_log(message) -> None:
    logging.debug(message)


def custom_readlines(handle, line_separator: str="\n", chunk_size: int=1):
    buf = ""
    while not handle.closed:
        data = handle.read(chunk_size)
        if not data:
            break
        buf += data.decode("utf-8")
        if line_separator in buf:
            chunks = buf.split(line_separator)
            buf = chunks.pop()
            for chunk in chunks:
                yield chunk + line_separator
        if buf.endswith("scons>>>"):
            yield buf
            buf = ""


def custom_readerr(handle, line_separator: str="\n", chunk_size: int=1):
    buf = ""
    while not handle.closed:
        data = handle.read(chunk_size)
        if not data:
            break
        buf += data.decode("utf-8")
        if line_separator in buf:
            chunks = buf.split(line_separator)
            buf = chunks.pop()
            for chunk in chunks:
                yield chunk + line_separator


def enqueue_output(out, queue) -> None:
    for line in iter(custom_readlines(out)):
        queue.put(line)
    out.close()


def enqueue_error(err, queue) -> None:
    for line in iter(custom_readerr(err)):
        queue.put(line)
    err.close()


input_q = queue.Queue()
output_q = queue.Queue()
error_q = queue.Queue()

building_cv = Condition()
error_cv = Condition()

class StateInfo:
    def __init__(self) -> None:
        self.thread_error = False
        self.finished_building = []
        self.error_nodes = []
        self.startup_failed = False
        self.startup_output = ''
        self.daemon_needs_to_shutdown = False
        self.httpd = None

shared_state = StateInfo()

def sigint_func(signum, frame) -> None:
    global shared_state
    shared_state.daemon_needs_to_shutdown = True

signal.signal(signal.SIGINT, sigint_func)


def daemon_thread_func():
    global shared_state
    try:
        args_list = args + ["--interactive"]
        daemon_log(f"Starting daemon with args: {' '.join(args_list)}")
        daemon_log(f"cwd: {os.getcwd()}")

        p = Popen(args_list, stdout=PIPE, stderr=PIPE, stdin=PIPE)

        t = threading.Thread(target=enqueue_output, args=(p.stdout, output_q))
        t.daemon = True
        t.start()

        te = threading.Thread(target=enqueue_error, args=(p.stderr, error_q))
        te.daemon = True
        te.start()

        daemon_ready = False
        
        building_node = None
        startup_complete = False

        # While scons interactive process is stil running...
        while p.poll() is None:

            # while there is scons output to process
            while True:
                try:
                    line = output_q.get(block=False, timeout=0.01)
                except queue.Empty:
                    # breaks out of the output processing loop
                    break
                else:
                    daemon_log("output: " + line.strip())
                    if not startup_complete:
                        shared_state.startup_output += line

                    if "scons: building terminated because of errors." in line:
                        error_output = ""
                        while True:
                            try:
                                error_output += error_q.get(block=False, timeout=0.01)
                            except queue.Empty:
                                break
                        shared_state.error_nodes += [{"node": building_node, "error": error_output}]
                        daemon_ready = True
                        building_node = None
                        with building_cv:
                            building_cv.notify()

                    elif line == "scons>>>":
                        shared_state.startup_output = ''
                        startup_complete = True

                        with error_q.mutex:
                            error_q.queue.clear()
                        daemon_ready = True
                        with building_cv:
                            building_cv.notify()
                        building_node = None

            # while there is input to process...
            while daemon_ready and not input_q.empty():

                try:
                    building_node = input_q.get(block=False, timeout=0.01)
                except queue.Empty:
                    break
                if "exit" in building_node:
                    daemon_log("input: " + "exit")
                    p.stdin.write(b"exit\n")
                    p.stdin.flush()
                    with building_cv:
                        shared_state.finished_building += [building_node]
                    daemon_ready = False
                    shared_state.daemon_needs_to_shutdown = True
                    break

                else:
                    input_command = "build " + building_node + "\n"
                    daemon_log("input: " + input_command.strip())

                    p.stdin.write(input_command.encode("utf-8"))
                    p.stdin.flush()
                    with building_cv:
                        shared_state.finished_building += [building_node]
                    daemon_ready = False

            if shared_state.daemon_needs_to_shutdown:
                break
            time.sleep(0.01)

        # our scons process is done, make sure we are shutting down in this case
        if not shared_state.daemon_needs_to_shutdown:
            if not startup_complete:
                shared_state.startup_failed = True
            shared_state.daemon_needs_to_shutdown = True

    except Exception:
        shared_state.thread_error = True
        daemon_log("SERVER ERROR: " + traceback.format_exc())
        raise


daemon_thread = threading.Thread(target=daemon_thread_func)
daemon_thread.daemon = True
daemon_thread.start()

logging.debug(
    f"Starting request server on port {port}, keep alive: {daemon_keep_alive}"
)

keep_alive_timer = timer()

def server_thread_func() -> None:
    global shared_state
    class S(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            global shared_state
            global keep_alive_timer
            try:
                gets = parse_qs(urlparse(self.path).query)
                
                # process a request from ninja for a node for scons to build. 
                # Currently this is a serial process because scons interactive is serial
                # is it was originally meant for a real human user to be providing input
                # parallel input was never implemented.
                build = gets.get("build")
                if build:
                    keep_alive_timer = timer()

                    daemon_log(f"Got request: {build[0]}")
                    input_q.put(build[0])

                    def pred() -> bool:
                        return build[0] in shared_state.finished_building

                    with building_cv:
                        building_cv.wait_for(pred)

                    for error_node in shared_state.error_nodes:
                        if error_node["node"] == build[0]:
                            self.send_response(500)
                            self.send_header("Content-type", "text/html")
                            self.end_headers()
                            self.wfile.write(error_node["error"].encode())
                            return

                    self.send_response(200)
                    self.send_header("Content-type", "text/html")
                    self.end_headers()
                    return

                # this message is used in server startup, to make sure the server launched
                # successfully. If SCons interactive got to a input prompt (scons>>>), then
                # the server is ready to start processing commands. Otherwise the server will
                # send an error response back to ninja and shut itself down.
                ready = gets.get("ready")
                if ready:
                    if shared_state.startup_failed:
                        self.send_response(500)
                        self.send_header("Content-type", "text/html")
                        self.end_headers()
                        self.wfile.write(shared_state.startup_output.encode())
                        return

                exitbuild = gets.get("exit")
                if exitbuild:
                    input_q.put("exit")

                self.send_response(200)
                self.send_header("Content-type", "text/html")
                self.end_headers()

            except Exception:
                shared_state.thread_error = True
                daemon_log("SERVER ERROR: " + traceback.format_exc())
                raise

            def log_message(self, format, *args) -> None:
                return

    socketserver.TCPServer.allow_reuse_address = True
    shared_state.httpd = socketserver.TCPServer(("127.0.0.1", port), S)
    shared_state.httpd.serve_forever()


server_thread = threading.Thread(target=server_thread_func)
server_thread.daemon = True
server_thread.start()

while (timer() - keep_alive_timer < daemon_keep_alive 
        and not shared_state.thread_error 
        and not shared_state.daemon_needs_to_shutdown):
    time.sleep(1)

if shared_state.thread_error:
    daemon_log(f"Shutting server on port {port} down because thread error.")
elif shared_state.daemon_needs_to_shutdown:
    daemon_log("Server shutting down upon request.")
else:
    daemon_log(
        f"Shutting server on port {port} down because timed out: {daemon_keep_alive}"
    )
shared_state.httpd.shutdown()
if os.path.exists(ninja_builddir / "scons_daemon_dirty"):
    os.unlink(ninja_builddir / "scons_daemon_dirty")
if os.path.exists(daemon_dir / "pidfile"):
    os.unlink(daemon_dir / "pidfile")

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
