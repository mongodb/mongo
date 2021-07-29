/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#import bisect
#import re
#import yaml
#from packaging.version import Version
##
## `args[0]` : the path to a `releases.yml` file.
## `args[1]` : the mongo version
##
#set $releases_yml_path = $args[0]
#set $mongo_version = $args[1]
##
#set $mvc_file = open($releases_yml_path, 'r')
#set $mvc_doc = yaml.safe_load($mvc_file)
#set $fcvs = $mvc_doc['featureCompatibilityVersions']
#set $majors = $mvc_doc['majorReleases']
##
## Transform strings to versions.
#set $fcvs = list(map(Version, $fcvs))
#set $majors = list(map(Version, $majors))
##
#set $latest = Version(re.sub(r'-.*', '', $mongo_version))
## Highest release less than latest.
#set $last_continuous = $fcvs[bisect.bisect_left($fcvs, $latest) - 1]
## Highest major release less than latest.
#set global $last_lts = $majors[bisect.bisect_left($majors, $latest) - 1]
##
## Format a Version as `{major}_{minor}`.
#def $underscores(v): ${'{}_{}'.format(v.major, v.minor)}
#def $fcv_prefix(v):${'kFullyDowngradedTo_' if v == $last_lts else 'kVersion_'}
#def $fcv_cpp_name(v):${'{}{}'.format($fcv_prefix(v), $underscores(v))}

namespace mongo::multiversion {

enum class FeatureCompatibilityVersion {
    kInvalid,
    kUnsetDefaultBehavior_$underscores($last_lts),
## Generate FCV constants for all versions from last_lts to latest, inclusive.
#for $fcv in $fcvs[bisect.bisect_left($fcvs, $last_lts):bisect.bisect_right($fcvs, $latest)]:
    $fcv_cpp_name($fcv),
#end for
};

## Calculate number of versions since v4.4.
constexpr size_t kSince_$underscores(Version('4.4')) = ${len($fcvs) - 1};

// Last LTS was "$last_lts".
constexpr size_t kSinceLastLTS = ${len($fcvs) - bisect.bisect_left($fcvs, $last_lts) - 1};

class GenericFCV {
#def define_fcv_alias($id, v):
static constexpr auto $id = FeatureCompatibilityVersion::$fcv_cpp_name(v);#slurp
#end def
public:
    $define_fcv_alias('kLatest', $latest)
    $define_fcv_alias('kLastContinuous', $last_continuous)
    $define_fcv_alias('kLastLTS', $last_lts)
};

}  // namespace mongo::multiversion

/* vim: set filetype=cpp: */
