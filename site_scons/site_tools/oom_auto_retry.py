# Copyright 2023 MongoDB Inc.
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

import SCons

import functools
import subprocess
import sys
import time
import random
import os
import re

from typing import Callable, List, Dict


def command_spawn_func(sh: str, escape: Callable[[str], str], cmd: str, args: List, env: Dict,
                       target: List, source: List):
    retries = 0
    success = False

    build_env = target[0].get_build_env()
    oom_messages = [
        re.compile(msg, re.MULTILINE | re.DOTALL)
        for msg in build_env.get('OOM_RETRY_MESSAGES', [])
    ]
    oom_returncodes = [int(returncode) for returncode in build_env.get('OOM_RETRY_RETURNCODES', [])]
    max_retries = build_env.get('OOM_RETRY_ATTEMPTS', 10)
    oom_max_retry_delay = build_env.get('OOM_RETRY_MAX_DELAY_SECONDS', 120)

    while not success and retries <= max_retries:

        try:
            start_time = time.time()
            if sys.platform[:3] == 'win':
                # have to use shell=True for windows because of https://github.com/python/cpython/issues/53908
                proc = subprocess.run(' '.join(args), env=env, close_fds=True, shell=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                      check=True)
            else:
                proc = subprocess.run([sh, '-c', ' '.join(args)], env=env, close_fds=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                      check=True)
        except subprocess.CalledProcessError as exc:
            print(f"{os.path.basename(__file__)} captured error:")
            print(exc.stdout)
            if any([re.findall(oom_message, exc.stdout) for oom_message in oom_messages]) or any(
                [oom_returncode == exc.returncode for oom_returncode in oom_returncodes]):
                retries += 1
                retry_delay = int((time.time() - start_time) +
                                  oom_max_retry_delay * random.random())
                print(f"Ran out of memory while trying to build {target[0]}", )
                if retries <= max_retries:
                    print(f"trying again in {retry_delay} seconds with retry attempt {retries}")
                    time.sleep(retry_delay)
                    continue

            # There was no OOM error or no more OOM retries left
            return exc.returncode
        else:
            if proc.stdout:
                print(proc.stdout)
            return proc.returncode


def generate(env):

    original_command_execute = SCons.Action.CommandAction.execute

    def oom_retry_execute(command_action_instance, target, source, env, executor=None):

        if 'conftest' not in str(target[0]) and target[0].has_builder() and target[0].get_builder(
        ).get_name(env) in [
                'Object', 'SharedObject', 'StaticObject', 'Program', 'StaticLibrary',
                'SharedLibrary'
        ]:

            original_spawn = env['SPAWN']

            env['SPAWN'] = functools.partial(command_spawn_func, target=target, source=source)
            result = original_command_execute(command_action_instance, target, source, env,
                                              executor)
            env['SPAWN'] = original_spawn

        else:
            result = original_command_execute(command_action_instance, target, source, env,
                                              executor)
        return result

    SCons.Action.CommandAction.execute = oom_retry_execute


def exists(env):
    return True
