#!/usr/bin/env python3
#
# Copyright 2020 MongoDB Inc.
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
#
"""
Libdeps Graph Visualization Tool.

Starts a web service which creates a UI for interacting and examining the libdeps graph.
The web service front end consist of React+Redux for the framework, flask API for backend
communication, and Material UI for the GUI. The web service back end uses flask.

This script will automatically install the npm modules, and build and run the production
web service if not debug.
"""

import os
from pathlib import Path
import argparse
import shutil
import subprocess
import platform
import threading
import copy
import textwrap

import flask
from graph_visualizer_web_stack.flask.flask_backend import BackendServer


def get_args():
    """Create the argparse and return passed args."""

    parser = argparse.ArgumentParser()

    parser.add_argument(
        '--debug', action='store_true', help=
        'Whether or not to run debug server. Note for non-debug, you must build the production frontend with "npm run build".'
    )
    parser.add_argument(
        '--graphml-dir', type=str, action='store', help=
        "Directory where libdeps graphml files live. The UI will allow selecting different graphs from this location",
        default="build/opt")

    parser.add_argument('--frontend-host', type=str, action='store',
                        help="Hostname where the front end will run.", default="localhost")

    parser.add_argument('--backend-host', type=str, action='store',
                        help="Hostname where the back end will run.", default="localhost")

    parser.add_argument('--frontend-port', type=str, action='store',
                        help="Port where the front end will run.", default="3000")

    parser.add_argument('--backend-port', type=str, action='store',
                        help="Port where the back end will run.", default="5000")

    parser.add_argument('--memory-limit', type=float, action='store',
                        help="Limit in GB for backend memory usage.", default=8.0)

    parser.add_argument('--launch', choices=['frontend', 'backend', 'both'], default='both',
                        help="Specifies which part of the web service to launch.")

    return parser.parse_args()


def execute_and_read_stdout(cmd, cwd, env):
    """Execute passed command and get realtime output."""

    popen = subprocess.Popen(cmd, stdout=subprocess.PIPE, cwd=str(cwd), env=env,
                             universal_newlines=True)
    for stdout_line in iter(popen.stdout.readline, ""):
        yield stdout_line
    popen.stdout.close()
    return_code = popen.wait()
    if return_code:
        raise subprocess.CalledProcessError(return_code, cmd)


def check_node(node_check, cwd):
    """Check node version and install npm packages."""

    status, output = subprocess.getstatusoutput(node_check)
    if status != 0 or not output.split('\n')[-1].startswith('v12'):
        print(
            textwrap.dedent(f"""\
            Failed to get node version 12 from 'node -v':
            output: '{output}'
            Perhaps run 'source {cwd}/setup_node_env.sh install'"""))
        exit(1)

    node_modules = cwd / 'node_modules'

    if not node_modules.exists():
        print(
            textwrap.dedent(f"""\
            {node_modules} not found, you need to run 'npm install' in {cwd}
            Perhaps run 'source {cwd}/setup_node_env.sh install'"""))
        exit(1)


def start_backend(web_service_info, debug):
    """Start the backend in debug mode."""

    web_service_info['app'].run(host=web_service_info['backend_host'],
                                port=web_service_info['backend_port'], debug=debug)


def start_frontend_thread(web_service_info, npm_command, debug):
    """Start the backend in debug mode."""
    env = os.environ.copy()
    backend_url = f"http://{web_service_info['backend_host']}:{web_service_info['backend_port']}"
    env['REACT_APP_API_URL'] = backend_url

    if debug:
        env['HOST'] = web_service_info['frontend_host']
        env['PORT'] = web_service_info['frontend_port']

        for output in execute_and_read_stdout(npm_command, cwd=web_service_info['cwd'], env=env):
            print(output, end="")
    else:
        for output in execute_and_read_stdout(npm_command, cwd=web_service_info['cwd'], env=env):
            print(output, end="")

        env['PATH'] = 'node_modules/.bin:' + env['PATH']
        react_frontend = subprocess.Popen([
            'http-server',
            'build',
            '-a',
            web_service_info['frontend_host'],
            '-p',
            web_service_info['frontend_port'],
            f"--cors={backend_url}",
        ], env=env, cwd=str(web_service_info['cwd']))
        stdout, stderr = react_frontend.communicate()
        print(f"frontend stdout: '{stdout}'\n\nfrontend stderr: '{stderr}'")


def main():
    """Start up the server."""

    args = get_args()

    # TODO: add https command line option and support
    server = BackendServer(graphml_dir=args.graphml_dir,
                           frontend_url=f"http://{args.frontend_host}:{args.frontend_port}",
                           memory_limit=args.memory_limit)

    app = server.get_app()
    cwd = Path(__file__).parent / 'graph_visualizer_web_stack'

    web_service_info = {
        'app': app,
        'cwd': cwd,
        'frontend_host': args.frontend_host,
        'frontend_port': args.frontend_port,
        'backend_host': args.backend_host,
        'backend_port': args.backend_port,
    }

    node_check = 'node -v'
    npm_start = ['npm', 'start']
    npm_build = ['npm', 'run', 'build']

    check_node(node_check, cwd)

    frontend_thread = None
    if args.launch in ['frontend', 'both']:
        if args.debug:
            npm_command = npm_start
        else:
            npm_command = npm_build

        frontend_thread = threading.Thread(target=start_frontend_thread,
                                           args=(web_service_info, npm_command, args.debug))
        frontend_thread.start()

    if args.launch in ['backend', 'both']:
        start_backend(web_service_info, args.debug)

    if frontend_thread:
        frontend_thread.join()


if __name__ == "__main__":
    main()
