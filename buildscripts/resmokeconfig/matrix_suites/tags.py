"""Dynamically Generated tags."""


# TODO SERVER-55857: Let this file be the single source of truth for tags. Remove dupe definitions.
# Base tag for all temporary multiversion exlcusions. Also can be used directly for
# exclusion from all suites.
class Tags(object):
    """Wrapper class for tags."""

    BACKPORT_REQUIRED_TAG = "backport_required_multiversion"

    from buildscripts.resmokelib.multiversionconstants import REQUIRES_FCV_TAG
    # Base exclusion tag list.
    EXCLUDE_TAGS_TEMPLATE = f"{REQUIRES_FCV_TAG},multiversion_incompatible,{BACKPORT_REQUIRED_TAG}"

    # TODO SERVER-55857: move this value to remsoke's internal config and move generate_exclude_yaml to resmoke.
    # Call generate_exclude_yaml() when fetching fixture files so we only reach out to github once and
    # call `mongo --version` once.
    EXCLUDE_TAGS_FILE = "multiversion_exclude_tags.yml"

    # TODO SERVER=55857: move to resmoke and get list of multiversion suites from resmoke.py
    MULTIVERSION_CONFIG_KEY = "use_in_multiversion"
