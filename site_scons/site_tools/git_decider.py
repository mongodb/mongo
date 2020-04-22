# Copyright 2020 MongoDB Inc.
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
#

def generate(env, **kwargs):

    # Grab the existing decider functions out of the environment
    # so we can invoke them when we can't use Git.
    base_decider = env.decide_target
    if base_decider != env.decide_source:
        raise Exception("Decider environment seems broken")

    from git import Git

    thisRepo = Git(env.Dir("#").abspath)
    currentGitState = thisRepo.ls_files("--stage")
    lines = currentGitState.split("\n")

    file_sha1_map = {}
    for line in lines:
        line_content = line.split()
        file_sha1_map[env.File(line_content[3]).path] = line_content[1]

    for m in thisRepo.ls_files("-m").split("\n"):
        if m:
            del file_sha1_map[env.File(m).path]

    def is_known_to_git(dependency):
        return str(dependency) in file_sha1_map

    def git_says_file_is_up_to_date(dependency, prev_ni):
        gitInfoForDep = file_sha1_map[str(dependency)]

        if prev_ni is None:
            dependency.get_ninfo().csig = gitInfoForDep
            return False

        if not (hasattr(prev_ni, "csig")):
            prev_ni.csig = gitInfoForDep

        result = gitInfoForDep == prev_ni.csig
        return result

    def MongoGitDecider(dependency, target, prev_ni, node):
        if not is_known_to_git(dependency):
            return base_decider(dependency, target, prev_ni, node)
        return not git_says_file_is_up_to_date(dependency, prev_ni)

    env.Decider(MongoGitDecider)


def exists(env):
    try:
        from git import Git

        Git(env.Dir("#").abspath).ls_files("--stage")
        return True
    except:
        return False
