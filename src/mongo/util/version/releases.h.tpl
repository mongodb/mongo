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

#raw
#pragma once

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#end raw
##
##
#from bisect import bisect_left, bisect_right
#import itertools
#import re
#import yaml
#from packaging.version import Version
##
## `args[0]` : the path to a `releases.yml` file.
## `args[1]` : the mongo version
##
#set releases_yml_path = $args[0]
#set mongo_version = $args[1]
##
#set mvc_file = open(releases_yml_path, 'r')
#set mvc_doc = yaml.safe_load(mvc_file)
#set mvc_fcvs = mvc_doc['featureCompatibilityVersions']
#set mvc_majors = mvc_doc['majorReleases']
##
## Transform strings to versions.
#set global fcvs = list(map(Version, mvc_fcvs))
#set majors = list(map(Version, mvc_majors))

#set global latest = Version(re.sub(r'-.*', '', mongo_version))
## Highest release less than latest.
#set global last_continuous = $fcvs[bisect_left($fcvs, $latest) - 1]
## Highest LTS release less than latest.
#set global last_lts = majors[bisect_left(majors, $latest) - 1]
##
#set global generic_fcvs = {'LastLTS': $last_lts, 'LastContinuous': $last_continuous, 'Latest': $latest}
##
## Format a Version as `{major}_{minor}`.
#def underscores(v): ${'{}_{}'.format(v.major, v.minor)}
#def fcv_prefix(v): ${'kFullyDowngradedTo_' if v == $last_lts else 'kVersion_'}
#def fcv_cpp_name(v): ${'{}{}'.format($fcv_prefix(v), $underscores(v))}
##
#def transition_enum_name(transition, first, second):
k$(transition)_$(underscores(first))_To_$(underscores(second))#slurp
#end def

namespace mongo::multiversion {
<%
fcvs = self.getVar('fcvs')
last_lts, last_continuous, latest = self.getVar('last_lts'), self.getVar('last_continuous'), self.getVar('latest')
generic_fcvs = self.getVar('generic_fcvs')

# The transition when used as a cpp variable.
down = 'DowngradingFrom'
up = 'UpgradingFrom'

# A list of (FCV enum name, FCV string) tuples.
fcv_list = []

for fcv_x in fcvs[bisect_left(fcvs, last_lts):bisect_right(fcvs, latest)]:
    fcv_list.append((self.fcv_cpp_name(fcv_x), fcv_x))
    if fcv_x in generic_fcvs.values():
        for fcv_y in filter(lambda y : y > fcv_x, generic_fcvs.values()):
            fcv_list.extend([(self.transition_enum_name(up, fcv_x, fcv_y),
                              f'upgrading from {fcv_x} to {fcv_y}'),
                             (self.transition_enum_name(down, fcv_y, fcv_x),
                              f'downgrading from {fcv_y} to {fcv_x}')])
%>
##
##
enum class FeatureCompatibilityVersion {
    kInvalid,
    kUnsetDefaultBehavior_$underscores($last_lts),

#for fcv, _ in fcv_list:
    $fcv,
#end for
};

## Calculate number of versions since v4.4.
constexpr size_t kSince_$underscores(Version('4.4')) = ${bisect_left(fcvs, latest)};

// Last LTS was "$last_lts".
constexpr size_t kSinceLastLTS = ${bisect_left(fcvs, latest) - bisect_left(fcvs, last_lts)};

class GenericFCV {
#def define_fcv_alias(id, v):
static constexpr auto $id = FeatureCompatibilityVersion::$fcv_cpp_name(v);#slurp
#end def
##
#def define_generic_transition_alias(transition, first_name, first_value, second_name, second_value):
static constexpr auto k$transition$(first_name)To$(second_name) = #slurp
FeatureCompatibilityVersion::$transition_enum_name(transition, first_value, second_value);#slurp
#end def
##
#def define_generic_invalid_alias(transition, first, second):
static constexpr auto k$transition$(first)To$(second) = FeatureCompatibilityVersion::kInvalid;#slurp
#end def
##
<%
generic_transitions = []
for fcv_x, fcv_y in itertools.permutations(generic_fcvs.items(), 2):
    if fcv_x[1] >= fcv_y[1]:
        continue
    generic_transitions.extend([
        self.define_generic_transition_alias(up, fcv_x[0], fcv_x[1], fcv_y[0], fcv_y[1]),
        self.define_generic_transition_alias(down, fcv_y[0], fcv_y[1], fcv_x[0], fcv_x[1])
    ])

# Generate "invalid" definitions for LastLTS <=> LastContinuous.
lts = 'LastLTS'
cont = 'LastContinuous'
if generic_fcvs[cont] == generic_fcvs[lts]:
    generic_transitions.extend([
        self.define_generic_invalid_alias(up, lts, cont),
        self.define_generic_invalid_alias(down, cont, lts)
    ])
%>
public:
    $define_fcv_alias('kLatest', latest)
    $define_fcv_alias('kLastContinuous', last_continuous)
    $define_fcv_alias('kLastLTS', last_lts)

#for fcv in generic_transitions:
    $fcv
#end for
};

inline StringData toString(FeatureCompatibilityVersion v) {
    switch (v) {
        case FeatureCompatibilityVersion::kInvalid:
            return "invalid"_sd;
        case FeatureCompatibilityVersion::kUnsetDefaultBehavior_$underscores(last_lts):
            return "unset"_sd;
#for fcv, fcv_string in fcv_list:
        case FeatureCompatibilityVersion::$fcv:
            return "$fcv_string"_sd;
#end for
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo::multiversion

/* vim: set filetype=cpp: */
