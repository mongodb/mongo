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
  cpp_includes:
  - "mongo/db/change_streams_cluster_parameter.h"

imports:
    - "mongo/db/basic_types.idl"
    - "mongo/idl/cluster_server_parameter.idl"

structs:
    CWSPIntStorage:
      description: "Storage used for cwspTestNeedsLatestFCV"
      inline_chained_structs: true
      chained_structs:
          ClusterServerParameter: ClusterServerParameter
      fields:
          intData:
              type: safeInt64
              default: 0

feature_flags:
    featureFlagToaster:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagToaster
      default: false
      shouldBeFCVGated: true

    featureFlagFryer:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagFryer
      default: false
      shouldBeFCVGated: true
    
    featureFlagFork:
      description: "Create a feature flag that should not be FCV gated"
      cpp_varname: gFeatureFlagFork
      default: true
      shouldBeFCVGated: false

#def $ver_str(v): ${'{}.{}'.format(v.major, v.minor)}
    featureFlagBlender:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagBlender
      default: true
      # The version should be a valid FCV not equal to GenericFCV::kLastLTS in
      # the generated 'releases.h' file.
      version: $ver_str(latest)
      shouldBeFCVGated: true

    featureFlagSpoon:
      description: "Create a feature flag"
      cpp_varname: gFeatureFlagSpoon
      default: true
      # The version should match GenericFCV::kLastLTS in the generated 'releases.h' file.
      version: $ver_str(last_lts)
      shouldBeFCVGated: true

server_parameters:
    spTestNeedsFeatureFlagToaster:
      description: "Server Parameter gated on featureFlagToaster"
      set_at: runtime
      cpp_varname: gSPTestFeatureFlagToaster
      cpp_vartype: bool
      test_only: true
      default: false
      condition:
        feature_flag: gFeatureFlagToaster

    spTestNeedsLatestFCV:
      description: "Server parameter gated on FCV >= latestFCV"
      set_at: runtime
      cpp_varname: gSPTestLatestFCV
      cpp_vartype: int
      test_only: true
      default: 0
      condition:
        min_fcv: $ver_str(latest)

    cwspTestNeedsLatestFCV:
      description: "Cluster server parameter gated on latest FCV"
      set_at: cluster
      cpp_varname: gCWSPTestLatestFCV
      cpp_vartype: CWSPIntStorage
      test_only: true
      condition:
        min_fcv: $ver_str(latest)

    spTestNeedsFeatureFlagBlender:
      description: "Server Parameter gated on featureFlagBlender"
      set_at: runtime
      cpp_varname: gSPTestFeatureFlagBlender
      cpp_vartype: int
      test_only: true
      default: 0
      condition:
        feature_flag: gFeatureFlagBlender

    cwspTestNeedsFeatureFlagBlender:
      description: "Cluster server Parameter gated on featureFlagBlender"
      set_at: cluster
      cpp_varname: gCWSPTestFeatureFlagBlender
      cpp_vartype: CWSPIntStorage
      test_only: true
      condition:
        feature_flag: gFeatureFlagBlender
