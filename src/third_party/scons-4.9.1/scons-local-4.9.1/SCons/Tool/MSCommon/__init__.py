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
Common functions for Microsoft Visual Studio and Visual C/C++.
"""


import SCons.Errors
import SCons.Platform.win32
import SCons.Util  # noqa: F401

from SCons.Tool.MSCommon.sdk import (  # noqa: F401
    mssdk_exists,
    mssdk_setup_env,
)

from SCons.Tool.MSCommon.vc import (  # noqa: F401
    msvc_exists,
    msvc_setup_env_tool,
    msvc_setup_env_once,
    msvc_version_to_maj_min,
    msvc_find_vswhere,
    msvc_sdk_versions,
    msvc_toolset_versions,
    msvc_toolset_versions_spectre,
    msvc_query_version_toolset,
    vswhere_register_executable,
    vswhere_get_executable,
    vswhere_freeze_executable,
)

from SCons.Tool.MSCommon.vs import (  # noqa: F401
    get_default_version,
    get_vs_by_version,
    merge_default_version,
    msvs_exists,
    query_versions,
)

from .MSVC.Policy import (  # noqa: F401
    msvc_set_notfound_policy,
    msvc_get_notfound_policy,
    msvc_set_scripterror_policy,
    msvc_get_scripterror_policy,
    msvc_notfound_policy_contextmanager,
    msvc_scripterror_policy_contextmanager,
)

from .MSVC.Exceptions import (  # noqa: F401
    VisualCException,
    MSVCInternalError,
    MSVCUserError,
    MSVCScriptExecutionError,
    MSVCVersionNotFound,
    MSVCSDKVersionNotFound,
    MSVCToolsetVersionNotFound,
    MSVCSpectreLibsNotFound,
    MSVCArgumentError,
)

from .vc import (  # noqa: F401
    MSVCUnsupportedHostArch,
    MSVCUnsupportedTargetArch,
    MSVCScriptNotFound,
    MSVCUseScriptError,
    MSVCUseSettingsError,
    VSWhereUserError,
)

from .MSVC.Util import (  # noqa: F401
    msvc_version_components,
    msvc_extended_version_components,
    msvc_sdk_version_components,
)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
