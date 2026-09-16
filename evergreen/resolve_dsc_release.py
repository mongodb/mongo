"""Resolve the old-side DSC release for the disagg multiversion suites.

Thin shim: the implementation (contract, policy, and the VersionSource
implementations) lives in
buildscripts/resmokelib/multiversion/dsc_release.py -- split there so the
evergreen directory stays script-only and to escape the evergreen-py package
shadowing this directory's name. This file exists so the task's invocation
(`${python} evergreen/resolve_dsc_release.py`, see the "do multiversion
selection" function in etc/evergreen_yml_components/definitions.yml) and the
references to that path keep working.
"""

import sys

from buildscripts.resmokelib.multiversion.dsc_release import main

if __name__ == "__main__":
    sys.exit(main())
