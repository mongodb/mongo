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
#import yaml
##
## `args[0]` : the path to a `releases.yml` file.
##
#set $mvc_file = open($args[0], 'r')
#set $mvc_doc = yaml.safe_load($mvc_file)
#set $fcvs = $mvc_doc['featureCompatibilityVersions']
#set $majors = $mvc_doc['majorReleases']
 
## Change a dotted version to an underscore-delimited version.
#def underscores($v): $v.replace('.', '_')

namespace mongo::multiversion {

enum class FeatureCompatibilityVersion {
    kInvalid,

    kUnsetDefaultBehavior_$underscores($fcvs[0]),
    kFullyDowngradedTo_$underscores($fcvs[0]),

#for $fcv in $fcvs[1:]
    kVersion_$underscores($fcv),
#end for
};

constexpr size_t kSince_$underscores('4.4') = ${len($fcvs) - 1};

// Last major was "$majors[-1]".
#set $last_major_fcv_idx = $fcvs.index($majors[-1])
constexpr size_t kSinceLastMajor = ${len($fcvs) - $last_major_fcv_idx - 1};

}  // namespace mongo::multiversion
/* vim: set filetype=cpp: */
