#import bisect
#import re
#import yaml
##
#from packaging.version import Version
##
#set $releases_yml_path = $args[0] ## The path to a `releases.yml` file.
#set $mongo_version = $args[1]     ## The value of MONGO_VERSION.
##
#set $releases_file = open($releases_yml_path, 'r')
#set $releases = yaml.safe_load($releases_file)
#set $lts_releases = list(map(Version, $releases['longTermSupportReleases']))
##
## Parse the "MAJOR.MINOR" version from mongo_version.
#set $latest = Version(re.match(r'^[0-9]+\.[0-9]+', $mongo_version).group(0))
##
## The most recent lts release prior to 'latest'.
#set $last_lts = lts_releases[bisect.bisect_left(lts_releases, latest) - 1]
##
global:
  cpp_namespace: "mongo::feature_flags"

feature_flags:
    featureFlagToaster:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagToaster
      default: false

    featureFlagFryer:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagFryer
      default: false

#def $ver_str(v): ${'{}.{}'.format(v.major, v.minor)}
    featureFlagBlender:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagBlender
      default: true
      # The version should be a valid FCV not equal to GenericFCV::kLastLTS in
      # the generated 'releases.h' file.
      version: $ver_str(latest)

    featureFlagSpoon:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagSpoon
      default: true
      # The version should match GenericFCV::kLastLTS in the generated 'releases.h' file.
      version: $ver_str(last_lts)
