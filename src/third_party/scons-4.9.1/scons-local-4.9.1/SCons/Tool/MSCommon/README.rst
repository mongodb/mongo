.. sectnum::

README - SCons.Tool.MSCommon
############################

.. contents:: **Table of Contents**
   :depth: 2
   :local:


Design Notes
============

* Public, user-callable functions and exception types are available via
  the ``SCons.Tool.MSCommon`` namespace.

* Some existing code has been moved from ``MSCommon/vc.py`` to the appropriate
  ``MSCommon/MSVC/<modulename>``.

* No functions from the MSVC module or its child modules are intended to be invoked directly.
  All functions of interest are made available via the ``SCons.Tool.MSCommon`` namespace.
  It is anticipated that more code may be moved in the future as new features are added.
  By exposing the public API through ``SCons.Tool.MSCommon`` there should not be a problem
  with code movement.

* Additional helper functions primarily used for the test suite were added to
  ``MSCommon/vc.py`` and are available via the ``SCons.Tool.MSCommon`` namespace.


MSVC Detection Priority
=======================

For msvc version specifications without an 'Exp' suffix, an express
installation is used only when no other installation is detected.

+---------+---------+----------------------------------------------------------+
| Product | VCVer   | Priority                                                 |
+=========+=========+==========================================================+
| VS2022  | 14.3    | Enterprise, Professional, Community, BuildTools          |
+---------+---------+----------------------------------------------------------+
| VS2019  | 14.2    | Enterprise, Professional, Community, BuildTools          |
+---------+---------+----------------------------------------------------------+
| VS2017  | 14.1    | Enterprise, Professional, Community, BuildTools, Express |
+---------+---------+----------------------------------------------------------+
| VS2017  | 14.1Exp | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2015  | 14.0    | [Develop, BuildTools, CmdLine], Express                  |
+---------+---------+----------------------------------------------------------+
| VS2015  | 14.0Exp | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2013  | 12.0    | Develop, Express                                         |
+---------+---------+----------------------------------------------------------+
| VS2013  | 12.0Exp | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2012  | 11.0    | Develop, Express                                         |
+---------+---------+----------------------------------------------------------+
| VS2012  | 11.0Exp | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2010  | 10.0    | Develop, Express                                         |
+---------+---------+----------------------------------------------------------+
| VS2010  | 10.0Exp | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2008  | 9.0     | Develop, VCForPython, Express                            |
+---------+---------+----------------------------------------------------------+
| VS2008  | 9.0Exp  | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2005  | 8.0     | Develop, Express                                         |
+---------+---------+----------------------------------------------------------+
| VS2005  | 8.0Exp  | Express                                                  |
+---------+---------+----------------------------------------------------------+
| VS2003  | 7.1     | Develop                                                  |
+---------+---------+----------------------------------------------------------+
| VS2002  | 7.0     | Develop                                                  |
+---------+---------+----------------------------------------------------------+
| VS6.0   | 6.0     | Develop                                                  |
+---------+---------+----------------------------------------------------------+

Legend:

  Develop
    devenv.com or msdev.com is detected.
  
  Express
    WDExpress.exe or VCExpress.exe is detected.
  
  BuildTools [VS2015]
    The vcvarsall batch file dispatches to the buildtools batch file.
  
  CmdLine [VS2015]
    Neither Develop, Express, or BuildTools.

VS2015 Edition Limitations
==========================

VS2015 BuildTools
-----------------

The VS2015 BuildTools stand-alone batch file does not support the ``sdk version`` argument.

The VS2015 BuildTools stand-alone batch file does not support the ``store`` argument.

These arguments appear to be silently ignored and likely would result in compiler
and/or linker build failures.

The VS2015 BuildTools ``vcvarsall.bat`` batch file dispatches to the stand-alone buildtools
batch file under certain circumstances. A fragment from the vcvarsall batch file is:

::

    if exist "%~dp0..\common7\IDE\devenv.exe" goto setup_VS
    if exist "%~dp0..\common7\IDE\wdexpress.exe" goto setup_VS
    if exist "%~dp0..\..\Microsoft Visual C++ Build Tools\vcbuildtools.bat" goto setup_buildsku

    :setup_VS

    ...

    :setup_buildsku
    if not exist "%~dp0..\..\Microsoft Visual C++ Build Tools\vcbuildtools.bat" goto usage
    set CurrentDir=%CD%
    call "%~dp0..\..\Microsoft Visual C++ Build Tools\vcbuildtools.bat" %1 %2
    cd /d %CurrentDir%
    goto :eof

VS2015 Express
--------------

The VS2015 Express batch file does not support the ``sdk version`` argument.

The VS2015 Express batch file does not support the ``store`` argument for the ``amd64`` and
``arm`` target architectures

amd64 Target Architecture
^^^^^^^^^^^^^^^^^^^^^^^^^

As installed, VS2015 Express does not support the ``store`` argument for the ``amd64`` target
architecture.  The generated ``store`` library paths include directories that do not exist.

The store library paths appear in two places in the ``vcvarsx86_amd64`` batch file:

::

    :setstorelib
    @if exist "%VCINSTALLDIR%LIB\amd64\store" set LIB=%VCINSTALLDIR%LIB\amd64\store;%LIB%
    ...
    :setstorelibpath
    @if exist "%VCINSTALLDIR%LIB\amd64\store" set LIBPATH=%VCINSTALLDIR%LIB\amd64\store;%LIBPATH%

The correct store library paths would be:

::

    :setstorelib
    @if exist "%VCINSTALLDIR%LIB\store\amd64" set LIB=%VCINSTALLDIR%LIB\store\amd64;%LIB%
    ...
    :setstorelibpath
    @if exist "%VCINSTALLDIR%LIB\store\amd64" set LIBPATH=%VCINSTALLDIR%LIB\store\amd64;%LIBPATH%

arm Target Architecture
^^^^^^^^^^^^^^^^^^^^^^^

As installed, VS2015 Express does not support the ``store`` argument for the ``arm`` target
architecture.  The generated ``store`` library paths include directories that do not exist.

The store library paths appear in two places in the ``vcvarsx86_arm`` batch file:

::

    :setstorelib
    @if exist "%VCINSTALLDIR%LIB\ARM\store" set LIB=%VCINSTALLDIR%LIB\ARM\store;%LIB%
    ...
    :setstorelibpath
    @if exist "%VCINSTALLDIR%LIB\ARM\store" set LIBPATH=%VCINSTALLDIR%LIB\ARM\store;%LIBPATH%

The correct store library paths would be file:

::

    :setstorelib
    @if exist "%VCINSTALLDIR%LIB\store\ARM" set LIB=%VCINSTALLDIR%LIB\store\ARM;%LIB%
    ...
    :setstorelibpath
    @if exist "%VCINSTALLDIR%LIB\store\ARM" set LIBPATH=%VCINSTALLDIR%LIB\store\ARM;%LIBPATH%


Known Issues
============

The following issues are known to exist:

* Using ``MSVC_USE_SCRIPT`` and ``MSVC_USE_SCRIPT_ARGS`` to call older Microsoft SDK
  ``SetEnv.cmd`` batch files may result in build failures.

  Typically, the reasons for build failures with SDK batch files are one, or both, of:

  * The batch files require delayed expansion to be enabled which is not usually the Windows default.

  * The batch files inspect environment variables that are not defined in the minimal subprocess
    environment in which the batch files are invoked.

* The code to suppress the "No versions of the MSVC compiler were found" warning for
  the default environment was moved from ``MSCommon/vc.py`` to ``MSCommon/MSVC/SetupEnvDefault.py``.
  There are very few, if any, existing unit tests. Now that the code is isolated in its own
  module with a limited API, unit tests may be easier to implement.


Experimental Features
=====================

msvc_query_version_toolset(version=None, prefer_newest=True, vswhere_exe=None)
------------------------------------------------------------------------------

The experimental function ``msvc_query_version_toolset`` was added to ``MSCommon/vc.py``
and is available via the ``SCons.Tool.MSCommon`` namespace.

This function takes a version specification or a toolset version specification, an optional product
preference, and an optional vswhere executable location as arguments and returns the msvc version and
the msvc toolset version for the corresponding version specification.

This is a proxy for using the toolset version for selection until that functionality can be added.

Example usage:

::

    for version in [
        '14.4',
        '14.3',
        '14.2',
        '14.1',
        '14.0',
        '14.32',
        '14.31',
        '14.29',
        '14.16',
        '14.00',
        '14.28.29333', # only 14.2
        '14.20.29333', # fictitious for testing
    ]:

        for prefer_newest in (True, False):
            try:
                msvc_version, msvc_toolset_version = msvc_query_version_toolset(version, prefer_newest=prefer_newest)
                failed = False
            except MSVCToolsetVersionNotFound:
                failed = True
            if failed:
                msg = 'FAILED'
                newline = '\n'
            else:
                env = Environment(MSVC_VERSION=msvc_version, MSVC_TOOLSET_VERSION=msvc_toolset_version)
                msg = 'passed'
                newline = ''
            print('{}Query: {} version={}, prefer_newest={}'.format(newline, msg, version, prefer_newest))

Example output fragment

::

    Build: _build003 {'MSVC_VERSION': '14.3', 'MSVC_TOOLSET_VERSION': '14.29.30133'}
    Where: C:\Software\MSVS-2022-143-Com\VC\Tools\MSVC\14.29.30133\bin\HostX64\x64\cl.exe
    Where: C:\Software\MSVS-2022-143-Com\Common7\Tools\guidgen.exe
    Query: passed version=14.2, prefer_newest=True

    Build: _build004 {'MSVC_VERSION': '14.2', 'MSVC_TOOLSET_VERSION': '14.29.30133'}
    Where: C:\Software\MSVS-2019-142-Com\VC\Tools\MSVC\14.29.30133\bin\HostX64\x64\cl.exe
    Where: C:\Software\MSVS-2019-142-Com\Common7\Tools\guidgen.exe
    Query: passed version=14.2, prefer_newest=False


Undocumented Features
=====================

set SCONS_CACHE_MSVC_FORCE_DEFAULTS=1
-------------------------------------

The Windows system environment variable ``SCONS_CACHE_MSVC_FORCE_DEFAULTS`` was added.  This variable is only
evaluated when the msvc cache is enabled and accepts the values ``1``, ``true``, and ``True``.

When enabled, the default msvc toolset version and the default sdk version, if not otherwise specified, are
added to the batch file argument list.  This is intended to make the cache more resilient to Visual Studio
updates that may change the default toolset version and/or the default SDK version.

Example usage:

::

    @echo Enabling scons cache ...
    @set "SCONS_CACHE_MSVC_CONFIG=mycachefile.json"
    @set "SCONS_CACHE_MSVC_FORCE_DEFAULTS=True"


End-User Diagnostic Tools
=========================

Due to the proliferation of user-defined msvc batch file arguments, the likelihood of end-user build
failures has increased.

Some of the options that may be employed in diagnosing end-user msvc build failures are listed below.

msvc_set_scripterror_policy('Warning') and MSVC_SCRIPTERROR_POLICY='Warning'
----------------------------------------------------------------------------

Enabling warnings to be produced for detected msvc batch file errors may provide additional context
for build failures. Refer to the documentation for details.

Change the default policy:

::

    from SCons.Tool.MSCommon import msvc_set_scripterror_policy

    msvc_set_scripterror_policy('Warning')

Specify the policy per-environment:

::

    env = Environment(MSVC_VERSION='14.3', MSVC_SPECTRE_LIBS=True, MSVC_SCRIPTERROR_POLICY='Warning')


set SCONS_MSCOMMON_DEBUG=mydebugfile.txt
----------------------------------------

The traditional method of diagnosing end-user issues is to enable the internal msvc debug logging.


set SCONS_CACHE_MSVC_CONFIG=mycachefile.json
--------------------------------------------

On occasion, enabling the cache file can prove to be a useful diagnostic tool.  If nothing else,
issues with the msvc environment may be readily apparent.


vswhere.exe
-----------

On occasion, the raw vswhere output may prove useful especially if there are suspected issues with
detection of installed msvc instances.

Windows command-line sample invocations:

::

    @rem 64-Bit Windows
    "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -all -sort -prerelease -products * -legacy -format json >MYVSWHEREOUTPUT.json

    @rem 32-Bit Windows:
    "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" -all -sort -prerelease -products * -legacy -format json >MYVSWHEREOUTPUT.json


Visual Studio Implementation Notes
==================================

Batch File Arguments
--------------------

Supported MSVC batch file arguments by product:

+---------+---------+--------+---------+---------+
| Product | UWP     | SDK    | Toolset | Spectre |
+=========+=========+========+=========+=========+
| VS2022  | X       | X      | X       | X       |
+---------+---------+--------+---------+---------+
| VS2019  | X       | X      | X       | X       |
+---------+---------+--------+---------+---------+
| VS2017  | X       | X      | X       | X       |
+---------+---------+--------+---------+---------+
| VS2015  | X [1]   | X [2]  |         |         |
+---------+---------+--------+---------+---------+

Notes:

1) The BuildTools edition does not support the ``store`` argument.  The Express edition
   supports the ``store`` argument for the ``x86`` target only.
2) The ``sdk version`` argument is not supported in the BuildTools and Express editions.

Supported MSVC batch file arguments in SCons:

+----------+----------------------------------------+-----------------------------------------------------+
| Argument | Construction Variable                  | Script Argument Equivalent                          |
+==========+========================================+=====================================================+
| UWP      | ``MSVC_UWP_APP=True``                  | ``MSVC_SCRIPT_ARGS='store'``                        |
+----------+----------------------------------------+-----------------------------------------------------+
| SDK      | ``MSVC_SDK_VERSION='10.0.20348.0'``    | ``MSVC_SCRIPT_ARGS='10.0.20348.0'``                 |
+----------+----------------------------------------+-----------------------------------------------------+
| Toolset  | ``MSVC_TOOLSET_VERSION='14.31.31103'`` | ``MSVC_SCRIPT_ARGS='-vcvars_ver=14.31.31103'``      |
+----------+----------------------------------------+-----------------------------------------------------+
| Spectre  | ``MSVC_SPECTRE_LIBS=True``             | ``MSVC_SCRIPT_ARGS='-vcvars_spectre_libs=spectre'`` |
+----------+----------------------------------------+-----------------------------------------------------+

**MSVC_SCRIPT_ARGS contents are not validated.  Utilizing script arguments that have construction
variable equivalents is discouraged and may lead to difficult to diagnose build errors.**

Additional constraints:

* ``MSVC_SDK_VERSION='8.1'`` and ``MSVC_UWP_APP=True`` is supported only for the v140
  build tools (i.e., ``MSVC_VERSION='14.0'`` or ``MSVC_TOOLSET_VERSION='14.0'``).

* ``MSVC_SPECTRE_LIBS=True`` and ``MSVC_UWP_APP=True`` is not supported (i.e., there
  are no spectre mitigations libraries for UWP builds).

Default Toolset Version
-----------------------

Side-by-side toolset versions were introduced in Visual Studio 2017.
The examples shown below are for Visual Studio 2022.

The msvc default toolset version is dependent on the installation options
selected.  This means that the default toolset version may be different for
each machine given the same Visual Studio product.

The msvc default toolset is not necessarily the latest toolset installed.
This has implications when a toolset version is specified using only one minor
digit (e.g., ``MSVC_TOOLSET_VERSION='14.3'`` or ``MSVC_SCRIPT_ARGS='-vcvars_ver=14.3'``).

Explicitly defining ``MSVC_TOOLSET_VERSION=None`` will return the same toolset
that the msvc batch files would return.  When using ``MSVC_SCRIPT_ARGS``, the
toolset specification should be omitted entirely.

Local installation and summary test results:

::

    VS2022\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt
        14.31.31103

    VS2022\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt
        14.32.31326

Toolset version summary:

::

    14.31.31103   Environment()
    14.31.31103   Environment(MSVC_TOOLSET_VERSION=None)

    14.32.31326*  Environment(MSVC_TOOLSET_VERSION='14.3')
    14.32.31326*  Environment(MSVC_SCRIPT_ARGS=['-vcvars_ver=14.3'])

    14.31.31103   Environment(MSVC_TOOLSET_VERSION='14.31')
    14.31.31103   Environment(MSVC_SCRIPT_ARGS=['-vcvars_ver=14.31'])

    14.32.31326   Environment(MSVC_TOOLSET_VERSION='14.32')
    14.32.31326   Environment(MSVC_SCRIPT_ARGS=['-vcvars_ver=14.32'])

VS2022\\Common7\\Tools\\vsdevcmd\\ext\\vcvars.bat usage fragment:

::

    @echo     -vcvars_ver=version : Version of VC++ Toolset to select
    @echo            ** [Default]   : If -vcvars_ver=version is NOT specified, the toolset specified by
    @echo                             [VSInstallDir]\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt will be used.
    @echo            ** 14.0        : VS 2015 (v140) VC++ Toolset (installation of the v140 toolset is a prerequisite)
    @echo            ** 14.xx       : VS 2017 or VS 2019 VC++ Toolset, if that version is installed on the system under
    @echo                             [VSInstallDir]\VC\MSVC\Tools\[version].  Where '14.xx' specifies a partial
    @echo                             [version]. The latest [version] directory that matches the specified value will
    @echo                             be used.
    @echo            ** 14.xx.yyyyy : VS 2017 or VS 2019 VC++ Toolset, if that version is installed on the system under
    @echo                             [VSInstallDir]\VC\MSVC\Tools\[version]. Where '14.xx.yyyyy' specifies an
    @echo                             exact [version] directory to be used.
    @echo            ** 14.xx.VV.vv : VS 2019 C++ side-by-side toolset package identity alias, if the SxS toolset has been installed on the system.
    @echo                             Where '14.xx.VV.vv' corresponds to a SxS toolset
    @echo                                 VV = VS Update Major Version (e.g. "16" for VS 2019 v16.9)
    @echo                                 vv = VS Update Minor version (e.g. "9" for VS 2019 v16.9)
    @echo                             Please see [VSInstallDir]\VC\Auxiliary\Build\[version]\Microsoft.VCToolsVersion.[version].txt for mapping of
    @echo                             SxS toolset to [VSInstallDir]\VC\MSVC\Tools\ directory.

VS2022 batch file fragment to determine the default toolset version:

::

    @REM Add MSVC
    set "__VCVARS_DEFAULT_CONFIG_FILE=%VCINSTALLDIR%Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"

    @REM We will "fallback" to Microsoft.VCToolsVersion.default.txt (latest) if Microsoft.VCToolsVersion.v143.default.txt does not exist.
    if EXIST "%VCINSTALLDIR%Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt" (
        if "%VSCMD_DEBUG%" GEQ "2" @echo [DEBUG:ext\%~nx0] Microsoft.VCToolsVersion.v143.default.txt was found.
        set "__VCVARS_DEFAULT_CONFIG_FILE=%VCINSTALLDIR%Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt"

    ) else (
        if "%VSCMD_DEBUG%" GEQ "1" @echo [DEBUG:ext\%~nx0] Microsoft.VCToolsVersion.v143.default.txt was not found. Defaulting to 'Microsoft.VCToolsVersion.default.txt'.
    )

Empirical evidence suggests that the default toolset version is different from the latest
toolset version when the toolset version immediately preceding the latest version is
installed.  For example, the ``14.31`` toolset version is installed when the ``14.32``
toolset version is the latest.


Visual Studio Version Notes
============================

SDK Versions
------------

+------+-------------------+
| SDK  | Format            |
+======+===================+
| 10.0 | 10.0.XXXXX.Y [1]  |
+------+-------------------+
| 8.1  | 8.1               |
+------+-------------------+

Notes:

1) The Windows 10 SDK version number is 10.0.20348.0 and earlier.

   The Windows 11 SDK version number is 10.0.22000.194 and later.

BuildSeries Versions
--------------------

+-------------+-------+-------+
| BuildSeries | VCVER | CLVER |
+=============+=======+=======+
| 14.4        | 14.4X | 19.4  |
+-------------+-------+-------+
| 14.3        | 14.3X | 19.3  |
+-------------+-------+-------+
| 14.2        | 14.2X | 19.2  |
+-------------+-------+-------+
| 14.1        | 14.1X | 19.1  |
+-------------+-------+-------+
| 14.0        | 14.0  | 19.0  |
+-------------+-------+-------+
| 12.0        | 12.0  | 18.0  |
+-------------+-------+-------+
| 11.0        | 11.0  | 17.0  |
+-------------+-------+-------+
| 10.0        | 10.0  | 16.0  |
+-------------+-------+-------+
| 9.0         | 9.0   | 15.0  |
+-------------+-------+-------+
| 8.0         | 8.0   | 14.0  |
+-------------+-------+-------+
| 7.1         | 7.1   | 13.1  |
+-------------+-------+-------+
| 7.0         | 7.0   | 13.0  |
+-------------+-------+-------+
| 6.0         | 6.0   | 12.0  |
+-------------+-------+-------+

BuildTools Versions
-------------------

+------------+-------------+----------+
| BuildTools | BuildSeries | MSVCRT   |
+============+=============+==========+
| v143       | 14.4, 14.3  | 140/ucrt |
+------------+-------------+----------+
| v142       | 14.2        | 140/ucrt |
+------------+-------------+----------+
| v141       | 14.1        | 140/ucrt |
+------------+-------------+----------+
| v140       | 14.0        | 140/ucrt |
+------------+-------------+----------+
| v120       | 12.0        | 120      |
+------------+-------------+----------+
| v110       | 11.0        | 110      |
+------------+-------------+----------+
| v100       | 10.0        | 100      |
+------------+-------------+----------+
| v90        | 9.0         | 90       |
+------------+-------------+----------+
| v80        | 8.0         | 80       |
+------------+-------------+----------+
| v71        | 7.1         | 71       |
+------------+-------------+----------+
| v70        | 7.0         | 70       |
+------------+-------------+----------+
| v60        | 6.0         | 60       |
+------------+-------------+----------+

Product Versions
----------------

+----------+-------+-------+-----------+------------------------+
| Product  | VSVER | SCons | SDK       | BuildTools             |
+==========+=======+=======+===========+========================+
| 2022     | 17.0  | 14.3  | 10.0, 8.1 | v143, v142, v141, v140 |
+----------+-------+-------+-----------+------------------------+
| 2019     | 16.0  | 14.2  | 10.0, 8.1 | v142, v141, v140       |
+----------+-------+-------+-----------+------------------------+
| 2017     | 15.0  | 14.1  | 10.0, 8.1 | v141, v140             |
+----------+-------+-------+-----------+------------------------+
| 2015     | 14.0  | 14.0  | 10.0, 8.1 | v140                   |
+----------+-------+-------+-----------+------------------------+
| 2013     | 12.0  | 12.0  |           | v120                   |
+----------+-------+-------+-----------+------------------------+
| 2012     | 11.0  | 11.0  |           | v110                   |
+----------+-------+-------+-----------+------------------------+
| 2010     | 10.0  | 10.0  |           | v100                   |
+----------+-------+-------+-----------+------------------------+
| 2008     | 9.0   | 9.0   |           | v90                    |
+----------+-------+-------+-----------+------------------------+
| 2005     | 8.0   | 8.0   |           | v80                    |
+----------+-------+-------+-----------+------------------------+
| 2003.NET | 7.1   | 7.1   |           | v71                    |
+----------+-------+-------+-----------+------------------------+
| 2002.NET | 7.0   | 7.0   |           | v70                    |
+----------+-------+-------+-----------+------------------------+
| 6.0      | 6.0   | 6.0   |           | v60                    |
+----------+-------+-------+-----------+------------------------+


SCons Implementation Notes
==========================

Compiler Detection Logic
------------------------

**WARNING: the compiler detection logic documentation below is likely out-of-date.**

In the future, the compiler detection logic documentation will be updated and integrated
into the current document format as appropriate.

::

    This is the flow of the compiler detection logic:

    External to MSCommon:

      The Tool init modules, in their exists() routines, call -> msvc_exists(env)

    At the moment, those modules are:
      SCons/Tool/midl.py
      SCons/Tool/mslib.py
      SCons/Tool/mslink.py
      SCons/Tool/msvc.py
      SCons/Tool/msvs.py

    env may contain a version request in MSVC_VERSION, but this is not used
    in the detection that follows from msvc_exists(), only in the later
    batch that starts with a call to msvc_setup_env().

    Internal to MSCommon/vc.py:

    + MSCommon/vc.py:msvc_exists:
    | vcs = cached_get_installed_vcs(env)
    | returns True if vcs > 0
    |
    +-> MSCommon/vc.py:cached_get_installed_vcs:
      | checks global if we've run previously, if so return it
      | populate the global from -> get_installed_vcs(env)
      |
      +-> MSCommon/vc.py:get_installed_vcs:
        | loop through "known" versions of msvc, granularity is maj.min
        |   check for product dir -> find_vc_pdir(env, ver)
        |
        +-> MSCommon/vc.py:find_vc_pdir:
          | From the msvc-version to pdir mapping dict, get reg key base and value
          | If value is none -> find_vc_pdir_vswhere(ver, env)
          |
          +-> MSCommon/vc.py:find_vc_pdir_vswhere:
            | From the vc-version to VS-version mapping table get string
            | Figure out where vswhere is -> msvc_find_vswhere()
            | Use subprocess to call vswhere, return first line of match
            /
          | else get product directory from registry (<= 14.0)
          /
        | if we found one -> _check_cl_exists_in_vc_dir(env, pdir, ver)
        |
        +-> MSCommon/vc.py:_check_cl_exists_in_vc_dir:
          | Figure out host/target pair
          | if version > 14.0 get specific version by looking in
          |    pdir + Auxiliary/Build/Microsoft/VCToolsVersion/default.txt
          |    look for pdir + Tools/MSVC/{specver}/bin/host/target/cl.exe
          | if 14.0 or less, "do older stuff"

    All of this just got us a yes-no answer on whether /some/ msvc version
    exists, but does populate __INSTALLED_VCS_RUN with all of the top-level
    versions as noted for get_installed_vcs

    Externally:

      Once a module's exists() has been called (or, in the case of
      clang/clangxx, after the compiler has been detected by other means -
      those still expect the rest of the msvc chain but not cl.exe)
      the module's generate() function calls -> msvc_setup_env_once(env)

    Internally:

    + MSCommon/vc.py:msvc_setup_env_once:
    | checks for environment flag MSVC_SETUP_RUN
    | if not, -> msvc_setup_env(env) and set flag
    |
    +-+ MSCommon/vc.py:msvc_setup_env:
      | set ver from -> get_default_version(env)
      |
      +-+ MSCommon/vc.py:get_default_version:
        | if no version specified in env.MSVC_VERSION:
        |   return first entry from -> cached_get_installed_vcs(env)
        | else return requested version
        /
      | get script from MSVC_USE_SCRIPT if set to a filename
      | -> script_env(script)
      |
      +-+ MSCommon/vc.py:script_env:
        | return (possibly cached) script variables matching script arg
        /
      | else -> msvc_find_valid_batch_script(env, version)
      |
      +-+ MSCommon/vc.py:msvc_find_valid_batch_script:
        | Build a list of plausible target values, and loop through
        |   look for host + target -> find_batch_file(env, ver, host, target)
        |
        +-+ MSCommon/vc.py:find_batch_file:
          | call -> find_vc_pdir (see above)
          | use the return to construct a version-biased batfile path, check
          /
        | if not found, try sdk scripts (unknown if this is still useful)


    Problems:
    - For VS >= 2017, VS and VS are not 1:1, there can be many VC for one VS
    - For vswhere-ready versions, detection does not proceed beyond the
      product level ("2019") into individual "features" (individual msvc)
    - As documented for MSVC_VERSION, compilers can only be requested if versions
      are from the set in _VCVER, so 14.1 but not 14.16 or 14.16.27023
    - Information found in the first pass (msvs_exists) isn't really
      available anywhere except the cached version list, since we just
      return true/false.
    - Since msvc_exists chain of calls does not look at version, we
      can proceed to compiler setup if *any* msvc was found, even if the
      one requested wasn't found.

