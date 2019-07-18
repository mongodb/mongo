# Copyright 2019 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import math
import os
import SCons

def exists(env):
    """Always enable"""
    ccache_path = env.get('CCACHE', env.WhereIs('ccache'))
    return os.path.exists(ccache_path)


def generate(env):
    """Add ccache support."""
    # ccache does not support response files so force scons to always
    # use the full command
    #
    # Note: This only works for Python versions >= 3.5
    env['MAXLINELENGTH'] = math.inf
    env['CCACHE'] = env.get('CCACHE', env.WhereIs('ccache'))
    env['CCCOM'] = '$CCACHE ' + env['CCCOM']
    env['CXXCOM'] = '$CCACHE ' + env['CXXCOM']
    env['SHCCCOM'] = '$CCACHE ' + env['SHCCCOM']
    env['SHCXXCOM'] = '$CCACHE ' + env['SHCXXCOM']


