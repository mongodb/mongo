# Copyright 2016 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# If available, uses Git metadata to decide whether files are out of date.

def generate(env, **kwargs):

    # Grab the existing decider functions out of the environment
    # so we can invoke them when we can't use Git.
    base_decider = env.decide_target
    if (base_decider != env.decide_source):
        raise Exception("Decider environment seems broken")

    from git import Git
    thisRepo = Git(env.Dir('#').abspath)
    currentGitState = thisRepo.ls_files('--stage')
    lines = currentGitState.split('\n')

    file_sha1_map = {}
    for line in lines:
        line_content = line.split()
        file_sha1_map[env.File(line_content[3]).path] = line_content[1]

    for m in thisRepo.ls_files('-m').split('\n'):
        if (m):
            del file_sha1_map[env.File(m).path]

    def is_known_to_git(dependency):
        return str(dependency) in file_sha1_map

    def git_says_file_is_up_to_date(dependency, prev_ni):
        gitInfoForDep = file_sha1_map[str(dependency)]

        if prev_ni is None:
            dependency.get_ninfo().csig = gitInfoForDep
            return False

        if not(hasattr(prev_ni, 'csig')):
            prev_ni.csig = gitInfoForDep

        result = gitInfoForDep == prev_ni.csig
        return result

    def MongoGitDecider(dependency, target, prev_ni):
        if not is_known_to_git(dependency):
            return base_decider(dependency, target, prev_ni)
        return not git_says_file_is_up_to_date(dependency, prev_ni)

    env.Decider(MongoGitDecider)

def exists(env):
    try:
        from git import Git
        Git(env.Dir('#').abspath).ls_files('--stage')
        return True
    except:
        return False
