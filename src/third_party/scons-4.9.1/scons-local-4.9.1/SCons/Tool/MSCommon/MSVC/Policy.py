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
Microsoft Visual C/C++ policy handlers.

Notes:
    * The default msvc not found policy is that a warning is issued. This can be
      changed globally via the function set_msvc_notfound_policy and/or through
      the environment via the MSVC_NOTFOUND_POLICY construction variable.
    * The default msvc script error policy is to suppress all msvc batch file
      error messages. This can be changed globally via the function
      set_msvc_scripterror_policy and/or through the environment via the
      MSVC_SCRIPTERROR_POLICY construction variable.
"""

from collections import (
    namedtuple,
)

from contextlib import (
    contextmanager,
)

import SCons.Warnings

from ..common import (
    debug,
)

from .Exceptions import (
    MSVCArgumentError,
    MSVCVersionNotFound,
    MSVCScriptExecutionError,
)

from .Warnings import (
    MSVCScriptExecutionWarning,
)

from . import Dispatcher
Dispatcher.register_modulename(__name__)


# MSVC_NOTFOUND_POLICY definition:
#     error:   raise exception
#     warning: issue warning and continue
#     ignore:  continue

MSVC_NOTFOUND_POLICY_DEFINITION = namedtuple('MSVCNotFoundPolicyDefinition', [
    'value',
    'symbol',
])

MSVC_NOTFOUND_DEFINITION_LIST = []

MSVC_NOTFOUND_POLICY_INTERNAL = {}
MSVC_NOTFOUND_POLICY_EXTERNAL = {}

for policy_value, policy_symbol_list in [
    (True,  ['Error',   'Exception']),
    (False, ['Warning', 'Warn']),
    (None,  ['Ignore',  'Suppress']),
]:

    policy_symbol = policy_symbol_list[0].lower()
    policy_def = MSVC_NOTFOUND_POLICY_DEFINITION(policy_value, policy_symbol)

    MSVC_NOTFOUND_DEFINITION_LIST.append(policy_def)

    MSVC_NOTFOUND_POLICY_INTERNAL[policy_symbol] = policy_def

    for policy_symbol in policy_symbol_list:
        MSVC_NOTFOUND_POLICY_EXTERNAL[policy_symbol.lower()] = policy_def
        MSVC_NOTFOUND_POLICY_EXTERNAL[policy_symbol] = policy_def
        MSVC_NOTFOUND_POLICY_EXTERNAL[policy_symbol.upper()] = policy_def

# default definition
_MSVC_NOTFOUND_POLICY_DEF = MSVC_NOTFOUND_POLICY_INTERNAL['warning']


# MSVC_SCRIPTERROR_POLICY definition:
#     error:   raise exception
#     warning: issue warning and continue
#     ignore:  continue

MSVC_SCRIPTERROR_POLICY_DEFINITION = namedtuple('MSVCBatchErrorPolicyDefinition', [
    'value',
    'symbol',
])

MSVC_SCRIPTERROR_DEFINITION_LIST = []

MSVC_SCRIPTERROR_POLICY_INTERNAL = {}
MSVC_SCRIPTERROR_POLICY_EXTERNAL = {}

for policy_value, policy_symbol_list in [
    (True,  ['Error',   'Exception']),
    (False, ['Warning', 'Warn']),
    (None,  ['Ignore',  'Suppress']),
]:

    policy_symbol = policy_symbol_list[0].lower()
    policy_def = MSVC_SCRIPTERROR_POLICY_DEFINITION(policy_value, policy_symbol)

    MSVC_SCRIPTERROR_DEFINITION_LIST.append(policy_def)

    MSVC_SCRIPTERROR_POLICY_INTERNAL[policy_symbol] = policy_def

    for policy_symbol in policy_symbol_list:
        MSVC_SCRIPTERROR_POLICY_EXTERNAL[policy_symbol.lower()] = policy_def
        MSVC_SCRIPTERROR_POLICY_EXTERNAL[policy_symbol] = policy_def
        MSVC_SCRIPTERROR_POLICY_EXTERNAL[policy_symbol.upper()] = policy_def

# default definition
_MSVC_SCRIPTERROR_POLICY_DEF = MSVC_SCRIPTERROR_POLICY_INTERNAL['ignore']


def _msvc_notfound_policy_lookup(symbol):

    try:
        notfound_policy_def = MSVC_NOTFOUND_POLICY_EXTERNAL[symbol]
    except KeyError:
        err_msg = "Value specified for MSVC_NOTFOUND_POLICY is not supported: {}.\n" \
                  "  Valid values are: {}".format(
                     repr(symbol),
                     ', '.join([repr(s) for s in MSVC_NOTFOUND_POLICY_EXTERNAL.keys()])
                  )
        raise MSVCArgumentError(err_msg)

    return notfound_policy_def

def msvc_set_notfound_policy(MSVC_NOTFOUND_POLICY=None):
    """ Set the default policy when MSVC is not found.

    Args:
        MSVC_NOTFOUND_POLICY:
           string representing the policy behavior
           when MSVC is not found or None

    Returns:
        The previous policy is returned when the MSVC_NOTFOUND_POLICY argument
        is not None. The active policy is returned when the MSVC_NOTFOUND_POLICY
        argument is None.

    """
    global _MSVC_NOTFOUND_POLICY_DEF

    prev_policy = _MSVC_NOTFOUND_POLICY_DEF.symbol

    policy = MSVC_NOTFOUND_POLICY
    if policy is not None:
        _MSVC_NOTFOUND_POLICY_DEF = _msvc_notfound_policy_lookup(policy)

    debug(
        'prev_policy=%s, set_policy=%s, policy.symbol=%s, policy.value=%s',
        repr(prev_policy), repr(policy),
        repr(_MSVC_NOTFOUND_POLICY_DEF.symbol), repr(_MSVC_NOTFOUND_POLICY_DEF.value)
    )

    return prev_policy

def msvc_get_notfound_policy():
    """Return the active policy when MSVC is not found."""
    debug(
        'policy.symbol=%s, policy.value=%s',
        repr(_MSVC_NOTFOUND_POLICY_DEF.symbol), repr(_MSVC_NOTFOUND_POLICY_DEF.value)
    )
    return _MSVC_NOTFOUND_POLICY_DEF.symbol

def msvc_notfound_handler(env, msg):

    if env and 'MSVC_NOTFOUND_POLICY' in env:
        # environment setting
        notfound_policy_src = 'environment'
        policy = env['MSVC_NOTFOUND_POLICY']
        if policy is not None:
            # user policy request
            notfound_policy_def = _msvc_notfound_policy_lookup(policy)
        else:
            # active global setting
            notfound_policy_def = _MSVC_NOTFOUND_POLICY_DEF
    else:
        # active global setting
        notfound_policy_src = 'default'
        policy = None
        notfound_policy_def = _MSVC_NOTFOUND_POLICY_DEF

    debug(
        'source=%s, set_policy=%s, policy.symbol=%s, policy.value=%s',
        notfound_policy_src, repr(policy), repr(notfound_policy_def.symbol), repr(notfound_policy_def.value)
    )

    if notfound_policy_def.value is None:
        # ignore
        pass
    elif notfound_policy_def.value:
        raise MSVCVersionNotFound(msg)
    else:
        SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, msg)

@contextmanager
def msvc_notfound_policy_contextmanager(MSVC_NOTFOUND_POLICY=None):
    """ Temporarily change the MSVC not found policy within a context.

    Args:
        MSVC_NOTFOUND_POLICY:
           string representing the policy behavior
           when MSVC is not found or None
    """
    prev_policy = msvc_set_notfound_policy(MSVC_NOTFOUND_POLICY)
    yield
    msvc_set_notfound_policy(prev_policy)

def _msvc_scripterror_policy_lookup(symbol):

    try:
        scripterror_policy_def = MSVC_SCRIPTERROR_POLICY_EXTERNAL[symbol]
    except KeyError:
        err_msg = "Value specified for MSVC_SCRIPTERROR_POLICY is not supported: {}.\n" \
                  "  Valid values are: {}".format(
                     repr(symbol),
                     ', '.join([repr(s) for s in MSVC_SCRIPTERROR_POLICY_EXTERNAL.keys()])
                  )
        raise MSVCArgumentError(err_msg)

    return scripterror_policy_def

def msvc_set_scripterror_policy(MSVC_SCRIPTERROR_POLICY=None):
    """ Set the default policy when msvc batch file execution errors are detected.

    Args:
        MSVC_SCRIPTERROR_POLICY:
           string representing the policy behavior
           when msvc batch file execution errors are detected or None

    Returns:
        The previous policy is returned when the MSVC_SCRIPTERROR_POLICY argument
        is not None. The active policy is returned when the MSVC_SCRIPTERROR_POLICY
        argument is None.

    """
    global _MSVC_SCRIPTERROR_POLICY_DEF

    prev_policy = _MSVC_SCRIPTERROR_POLICY_DEF.symbol

    policy = MSVC_SCRIPTERROR_POLICY
    if policy is not None:
        _MSVC_SCRIPTERROR_POLICY_DEF = _msvc_scripterror_policy_lookup(policy)

    debug(
        'prev_policy=%s, set_policy=%s, policy.symbol=%s, policy.value=%s',
        repr(prev_policy), repr(policy),
        repr(_MSVC_SCRIPTERROR_POLICY_DEF.symbol), repr(_MSVC_SCRIPTERROR_POLICY_DEF.value)
    )

    return prev_policy

def msvc_get_scripterror_policy():
    """Return the active policy when msvc batch file execution errors are detected."""
    debug(
        'policy.symbol=%s, policy.value=%s',
        repr(_MSVC_SCRIPTERROR_POLICY_DEF.symbol), repr(_MSVC_SCRIPTERROR_POLICY_DEF.value)
    )
    return _MSVC_SCRIPTERROR_POLICY_DEF.symbol

def msvc_scripterror_handler(env, msg):

    if env and 'MSVC_SCRIPTERROR_POLICY' in env:
        # environment setting
        scripterror_policy_src = 'environment'
        policy = env['MSVC_SCRIPTERROR_POLICY']
        if policy is not None:
            # user policy request
            scripterror_policy_def = _msvc_scripterror_policy_lookup(policy)
        else:
            # active global setting
            scripterror_policy_def = _MSVC_SCRIPTERROR_POLICY_DEF
    else:
        # active global setting
        scripterror_policy_src = 'default'
        policy = None
        scripterror_policy_def = _MSVC_SCRIPTERROR_POLICY_DEF

    debug(
        'source=%s, set_policy=%s, policy.symbol=%s, policy.value=%s',
        scripterror_policy_src, repr(policy), repr(scripterror_policy_def.symbol), repr(scripterror_policy_def.value)
    )

    if scripterror_policy_def.value is None:
        # ignore
        pass
    elif scripterror_policy_def.value:
        raise MSVCScriptExecutionError(msg)
    else:
        SCons.Warnings.warn(MSVCScriptExecutionWarning, msg)

@contextmanager
def msvc_scripterror_policy_contextmanager(MSVC_SCRIPTERROR_POLICY=None):
    """ Temporarily change the msvc batch execution errors policy within a context.

    Args:
        MSVC_SCRIPTERROR_POLICY:
           string representing the policy behavior
           when msvc batch file execution errors are detected or None
    """
    prev_policy = msvc_set_scripterror_policy(MSVC_SCRIPTERROR_POLICY)
    yield
    msvc_set_scripterror_policy(prev_policy)

