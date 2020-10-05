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

import os
import re
import subprocess
import urllib

from pkg_resources import parse_version

import SCons

_icecream_version_min = parse_version("1.1rc2")
_icecream_version_gcc_remote_cpp = parse_version("1.2")


# I'd prefer to use value here, but amazingly, its __str__ returns the
# *initial* value of the Value and not the built value, if
# available. That seems like a bug. In the meantime, make our own very
# sinmple Substition thing.
class _BoundSubstitution:
    def __init__(self, env, expression):
        self.env = env
        self.expression = expression
        self.result = None

    def __str__(self):
        if self.result is None:
            self.result = self.env.subst(self.expression)
        return self.result


def icecc_create_env(env, target, source, for_signature):
    # Safe to assume unix here because icecream only works on Unix
    mkdir = "mkdir -p ${TARGET.dir}"

    # Create the env, use awk to get just the tarball name and we store it in
    # the shell variable $ICECC_VERSION_TMP so the subsequent mv command and
    # store it in a known location. Add any files requested from the user environment.
    create_env = "ICECC_VERSION_TMP=$$(${SOURCES[0]} --$ICECC_COMPILER_TYPE ${SOURCES[1]} ${SOURCES[2]}"

    # TODO: It would be a little more elegant if things in
    # ICECC_CREATE_ENV_ADDFILES were handled as sources, because we
    # would get automatic dependency tracking. However, there are some
    # wrinkles around the mapped case so we have opted to leave it as
    # just interpreting the env for now.
    for addfile in env.get('ICECC_CREATE_ENV_ADDFILES', []):
        if isinstance(addfile, tuple):
            if len(addfile) == 2:
                if env['ICECREAM_VERSION'] > parse_version('1.1'):
                    raise Exception("This version of icecream does not support addfile remapping.")
                create_env += " --addfile {}={}".format(
                    env.File(addfile[0]).srcnode().abspath,
                    env.File(addfile[1]).srcnode().abspath)
                env.Depends(target, addfile[1])
            else:
                raise Exception(f"Found incorrect icecream addfile format: {str(addfile)}" +
                                f"\ntuple must two elements of the form" +
                                f"\n('chroot dest path', 'source file path')")
        else:
            try:
                create_env += f" --addfile {env.File(addfile).srcnode().abspath}"
                env.Depends(target, addfile)
            except:
                # NOTE: abspath is required by icecream because of
                # this line in icecc-create-env:
                # https://github.com/icecc/icecream/blob/10b9468f5bd30a0fdb058901e91e7a29f1bfbd42/client/icecc-create-env.in#L534
                # which cuts out the two files based off the equals sign and
                # starting slash of the second file
                raise Exception(f"Found incorrect icecream addfile format: {type(addfile)}" +
                                f"\nvalue provided cannot be converted to a file path")

    create_env += " | awk '/^creating .*\\.tar\\.gz/ { print $$2 }')"

    # Simply move our tarball to the expected locale.
    mv = "mv $$ICECC_VERSION_TMP $TARGET"

    # Daisy chain the commands and then let SCons Subst in the rest.
    cmdline = f"{mkdir} && {create_env} && {mv}"
    return cmdline


def generate(env):
    # icecc lower then 1.1 supports addfile remapping accidentally
    # and above it adds an empty cpuinfo so handle cpuinfo issues for icecream
    # below version 1.1
    if (env['ICECREAM_VERSION'] <= parse_version('1.1')
        and env.ToolchainIs("clang")
        and os.path.exists('/proc/cpuinfo')):
        env.AppendUnique(ICECC_CREATE_ENV_ADDFILES=[('/proc/cpuinfo', '/dev/null')])

    # Absoluteify, so we can derive ICERUN
    env["ICECC"] = env.WhereIs("$ICECC")

    if "ICERUN" in env:
        # Absoluteify, for parity with ICECC
        icerun = env.WhereIs("$ICERUN")
    else:
        icerun = env.File("$ICECC").File("icerun")
    env["ICERUN"] = icerun

    if "ICECC_CREATE_ENV" in env:
        icecc_create_env_bin = env.WhereIs("$ICECC_CREATE_ENV")
    else:
        icecc_create_env_bin = env.File("ICECC").File("icecc-create-env")
    env["ICECC_CREATE_ENV"] = icecc_create_env_bin

    # Make CC and CXX absolute paths too. This ensures the correct paths to
    # compilers get passed to icecc-create-env rather than letting it
    # potentially discover something we don't expect via PATH.
    env["CC"] = env.WhereIs("$CC")
    env["CXX"] = env.WhereIs("$CXX")

    # Set up defaults for configuration options
    env['ICECREAM_TARGET_DIR'] = env.Dir(
        env.get('ICECREAM_TARGET_DIR', '#./.icecream')
    )
    verbose = env.get('ICECREAM_VERBOSE', False)
    env['ICECREAM_DEBUG'] = env.get('ICECREAM_DEBUG', False)

    # We have a lot of things to build and run that the final user
    # environment doesn't need to see or know about. Make a custom env
    # that we use consistently from here to where we end up setting
    # ICECREAM_RUN_ICECC in the user env.
    setupEnv = env.Clone(
        NINJA_SKIP=True
    )

    if 'ICECC_VERSION' in setupEnv and bool(setupEnv['ICECC_VERSION']):

        if setupEnv["ICECC_VERSION"].startswith("http"):

            quoted = urllib.parse.quote(setupEnv['ICECC_VERSION'], safe=[])

            # Use curl / wget to download the toolchain because SCons (and ninja)
            # are better at running shell commands than Python functions.
            #
            # TODO: This all happens SCons side now. Should we just use python to
            # fetch instead?
            curl = setupEnv.WhereIs("curl")
            wget = setupEnv.WhereIs("wget")

            if curl:
                cmdstr = "curl -L"
            elif wget:
                cmdstr = "wget"
            else:
                raise Exception(
                    "You have specified an ICECC_VERSION that is a URL but you have neither wget nor curl installed."
                )

            # Copy ICECC_VERSION into ICECC_VERSION_URL so that we can
            # change ICECC_VERSION without perturbing the effect of
            # the action.
            setupEnv['ICECC_VERSION_URL'] = setupEnv['ICECC_VERSION']
            setupEnv['ICECC_VERSION'] = icecc_version_file = setupEnv.Command(
                target=f"$ICECREAM_TARGET_DIR/{quoted}",
                source=[setupEnv.Value(quoted)],
                action=SCons.Action.Action(
                    f"{cmdstr} -o $TARGET $ICECC_VERSION_URL",
                    "Downloading compiler package from $ICECC_VERSION_URL" if not verbose else str(),
                ),
            )[0]

        else:
            # Convert the users selection into a File node and do some basic validation
            setupEnv['ICECC_VERSION'] = icecc_version_file = setupEnv.File('$ICECC_VERSION')

            if not icecc_version_file.exists():
                raise Exception(
                    'The ICECC_VERSION variable set set to {}, but this file does not exist'.format(icecc_version_file)
                )

        # This is what we are going to call the file names as known to SCons on disk
        setupEnv["ICECC_VERSION_ID"] = "user_provided." + icecc_version_file.name

    else:

        setupEnv["ICECC_COMPILER_TYPE"] = setupEnv.get(
            "ICECC_COMPILER_TYPE", os.path.basename(setupEnv.WhereIs("${CC}"))
        )

        # This is what we are going to call the file names as known to SCons on disk. We do the
        # subst early so that we can call `replace` on the result.
        setupEnv["ICECC_VERSION_ID"] = setupEnv.subst(
            "icecc-create-env.${CC}${CXX}.tar.gz").replace("/", "_"
        )

        setupEnv["ICECC_VERSION"] = icecc_version_file = setupEnv.Command(
            target="$ICECREAM_TARGET_DIR/$ICECC_VERSION_ID",
            source=[
                "$ICECC_CREATE_ENV",
                "$CC",
                "$CXX"
            ],
            action=SCons.Action.Action(
                icecc_create_env,
                "Generating icecream compiler package: $TARGET" if not verbose else str(),
                generator=True,
            )
        )[0]

    # At this point, all paths above have produced a file of some sort. We now move on
    # to producing our own signature for this local file.

    setupEnv.Append(
        ICECREAM_TARGET_BASE_DIR='$ICECREAM_TARGET_DIR',
        ICECREAM_TARGET_BASE_FILE='$ICECC_VERSION_ID',
        ICECREAM_TARGET_BASE='$ICECREAM_TARGET_BASE_DIR/$ICECREAM_TARGET_BASE_FILE',
    )

    # If the file we are planning to use is not within
    # ICECREAM_TARGET_DIR then make a local copy of it that is.
    if icecc_version_file.dir != env['ICECREAM_TARGET_DIR']:
        setupEnv["ICECC_VERSION"] = icecc_version_file = setupEnv.Command(
            target=[
                '${ICECREAM_TARGET_BASE}.local',
            ],
            source=icecc_version_file,
            action=SCons.Defaults.Copy('$TARGET', '$SOURCE'),
        )

        # There is no point caching the copy.
        setupEnv.NoCache(icecc_version_file)

    # Now, we compute our own signature of the local compiler package,
    # and create yet another link to the compiler package with a name
    # containing our computed signature. Now we know that we can give
    # this filename to icecc and it will be assured to really reflect
    # the contents of the package, and not the arbitrary naming of the
    # file as found on the users filesystem or from
    # icecc-create-env. We put the absolute path to that filename into
    # a file that we can read from.
    icecc_version_info = setupEnv.File(setupEnv.Command(
        target=[
            '${ICECREAM_TARGET_BASE}.sha256',
            '${ICECREAM_TARGET_BASE}.sha256.path',
        ],
        source=icecc_version_file,
        action=SCons.Action.ListAction(
            [

                # icecc-create-env run twice with the same input will
                # create files with identical contents, and identical
                # filenames, but with different hashes because it
                # includes timestamps. So we compute a new hash based
                # on the actual stream contents of the file by
                # untarring it into shasum.
                SCons.Action.Action(
                    "tar xfO ${SOURCES[0]} | shasum -b -a 256 - | awk '{ print $1 }' > ${TARGETS[0]}",
                    "Calculating sha256 sum of ${SOURCES[0]}" if not verbose else str(),
                ),

                SCons.Action.Action(
                    "ln -f ${SOURCES[0]} ${TARGETS[0].dir}/icecream_py_sha256_$$(cat ${TARGETS[0]}).tar.gz",
                    "Linking ${SOURCES[0]} to its sha256 sum name" if not verbose else str(),
                ),

                SCons.Action.Action(
                    "echo ${TARGETS[0].dir.abspath}/icecream_py_sha256_$$(cat ${TARGETS[0]}).tar.gz > ${TARGETS[1]}",
                    "Storing sha256 sum name for ${SOURCES[0]} to ${TARGETS[1]}" if not verbose else str(),
                )
            ],
        )
    ))

    # We can't allow these to interact with the cache because the
    # second action produces a file unknown to SCons. If caching were
    # permitted, the other two files could be retrieved from cache but
    # the file produced by the second action could not (and would not)
    # be. We would end up with a broken setup.
    setupEnv.NoCache(icecc_version_info)

    # Create a value node that, when built, contains the result of
    # reading the contents of the sha256.path file. This way we can
    # pull the value out of the file and substitute it into our
    # wrapper script.
    icecc_version_string_value = setupEnv.Command(
        target=setupEnv.Value(None),
        source=[
            icecc_version_info[1]
        ],
        action=SCons.Action.Action(
            lambda env, target, source: target[0].write(source[0].get_text_contents()),
            "Reading compiler package sha256 sum path from $SOURCE" if not verbose else str(),
        )
    )[0]

    def icecc_version_string_generator(source, target, env, for_signature):
        if for_signature:
            return icecc_version_string_value.get_csig()
        return icecc_version_string_value.read()

    # Set the values that will be interpolated into the run-icecc script.
    setupEnv['ICECC_VERSION'] = icecc_version_string_generator

    # If necessary, we include the users desired architecture in the
    # interpolated file.
    icecc_version_arch_string = str()
    if "ICECC_VERSION_ARCH" in setupEnv:
        icecc_version_arch_string = "${ICECC_VERSION_ARCH}:"

    # Finally, create the run-icecc wrapper script. The contents will
    # re-invoke icecc with our sha256 sum named file, ensuring that we
    # trust the signature to be appropriate. In a pure SCons build, we
    # actually wouldn't need this Substfile, we could just set
    # env['ENV]['ICECC_VERSION'] to the Value node above. But that
    # won't work for Ninja builds where we can't ask for the contents
    # of such a node easily. Creating a Substfile means that SCons
    # will take care of generating a file that Ninja can use.
    run_icecc = setupEnv.Textfile(
        target="$ICECREAM_TARGET_DIR/run-icecc.sh",
        source=[
            '#!/bin/sh',
            'ICECC_VERSION=@icecc_version_arch@@icecc_version@ exec @icecc@ "$@"',
            '',
        ],
        SUBST_DICT={
            '@icecc@' : '$ICECC',
            '@icecc_version@' : '$ICECC_VERSION',
            '@icecc_version_arch@' : icecc_version_arch_string,
        },

        # Don't change around the suffixes
        TEXTFILEPREFIX=str(),
        TEXTFILESUFFIX=str(),

        # Somewhat surprising, but even though Ninja will defer to
        # SCons to invoke this, we still need ninja to be aware of it
        # so that it knows to invoke SCons to produce it as part of
        # TEMPLATE expansion. Since we have set NINJA_SKIP=True for
        # setupEnv, we need to reverse that here.
        NINJA_SKIP=False
    )

    setupEnv.AddPostAction(
        run_icecc,
        action=SCons.Defaults.Chmod('$TARGET', "u+x"),
    )

    setupEnv.Depends(
        target=run_icecc,
        dependency=[

            # TODO: Without the ICECC dependency, changing ICECC doesn't cause the Substfile
            # to regenerate. Why is this?
            '$ICECC',

            # This dependency is necessary so that we build into this
            # string before we create the file.
            icecc_version_string_value,

            # TODO: SERVER-50587 We need to make explicit depends here because of NINJA_SKIP. Any
            # dependencies in the nodes created in setupEnv with NINJA_SKIP would have
            # that dependency chain hidden from ninja, so they won't be rebuilt unless
            # added as dependencies here on this node that has NINJA_SKIP=False.
            '$CC',
            '$CXX',
            icecc_version_file,
        ],
    )

    # From here out, we make changes to the users `env`.
    setupEnv = None

    env['ICECREAM_RUN_ICECC'] = run_icecc[0]

    def icecc_toolchain_dependency_emitter(target, source, env):
        if "conftest" not in str(target[0]):
            # Requires or Depends? There are trade-offs:
            #
            # If it is `Depends`, then enabling or disabling icecream
            # will cause a global recompile. But, if you regenerate a
            # new compiler package, you will get a rebuild. If it is
            # `Requires`, then enabling or disabling icecream will not
            # necessarily cause a global recompile (it depends if
            # C[,C,XX]FLAGS get changed when you do so), but on the
            # other hand if you regenerate a new compiler package you
            # will *not* get a rebuild.
            #
            # For now, we are opting for `Requires`, because it seems
            # preferable that opting in or out of icecream shouldn't
            # force a rebuild.
            env.Requires(target, "$ICECREAM_RUN_ICECC")
        return target, source

    # Cribbed from Tool/cc.py and Tool/c++.py. It would be better if
    # we could obtain this from SCons.
    _CSuffixes = [".c"]
    if not SCons.Util.case_sensitive_suffixes(".c", ".C"):
        _CSuffixes.append(".C")

    _CXXSuffixes = [".cpp", ".cc", ".cxx", ".c++", ".C++"]
    if SCons.Util.case_sensitive_suffixes(".c", ".C"):
        _CXXSuffixes.append(".C")

    suffixes = _CSuffixes + _CXXSuffixes
    for object_builder in SCons.Tool.createObjBuilders(env):
        emitterdict = object_builder.builder.emitter
        for suffix in emitterdict.keys():
            if not suffix in suffixes:
                continue
            base = emitterdict[suffix]
            emitterdict[suffix] = SCons.Builder.ListEmitter(
                [base, icecc_toolchain_dependency_emitter]
            )

    # Check whether ccache is requested and is a valid tool.
    if "CCACHE" in env:
        ccache = SCons.Tool.Tool('ccache')
        ccache_enabled = bool(ccache) and ccache.exists(env)
    else:
        ccache_enabled = False

    if env.ToolchainIs("clang"):
        env["ENV"]["ICECC_CLANG_REMOTE_CPP"] = 1
    elif env.ToolchainIs("gcc"):
        if env["ICECREAM_VERSION"] < _icecream_version_gcc_remote_cpp:
            # We aren't going to use ICECC_REMOTE_CPP because icecc
            # 1.1 doesn't offer it. We disallow fallback to local
            # builds because the fallback is serial execution.
            env["ENV"]["ICECC_CARET_WORKAROUND"] = 0
        elif not ccache_enabled:
            # If we can, we should make Icecream do its own preprocessing
            # to reduce concurrency on the local host. We should not do
            # this when ccache is in use because ccache will execute
            # Icecream to do its own preprocessing and then execute
            # Icecream as the compiler on the preprocessed source.
            env["ENV"]["ICECC_REMOTE_CPP"] = 1

    if "ICECC_SCHEDULER" in env:
        env["ENV"]["USE_SCHEDULER"] = env["ICECC_SCHEDULER"]

    # If ccache is in play we actually want the icecc binary in the
    # CCACHE_PREFIX environment variable, not on the command line, per
    # the ccache documentation on compiler wrappers. Otherwise, just
    # put $ICECC on the command line. We wrap it in the magic "don't
    # consider this part of the build signature" sigils in the hope
    # that enabling and disabling icecream won't cause rebuilds. This
    # is unlikely to really work, since above we have maybe changed
    # compiler flags (things like -fdirectives-only), but we still try
    # to do the right thing.
    if ccache_enabled:
        # If the path to CCACHE_PREFIX isn't absolute, then it will
        # look it up in PATH. That isn't what we want here, we make
        # the path absolute.
        env['ENV']['CCACHE_PREFIX'] = _BoundSubstitution(env, "${ICECREAM_RUN_ICECC.abspath}")
    else:
        # Make a generator to expand to ICECC in the case where we are
        # not a conftest. We never want to run conftests remotely.
        # Ideally, we would do this for the CCACHE_PREFIX case above,
        # but unfortunately if we did we would never actually see the
        # conftests, because the BoundSubst means that we will never
        # have a meaningful `target` variable when we are in ENV.
        # Instead, rely on the ccache.py tool to do it's own filtering
        # out of conftests.
        def icecc_generator(target, source, env, for_signature):
            if "conftest" not in str(target[0]):
                return '$ICECREAM_RUN_ICECC'
            return ''
        env['ICECC_GENERATOR'] = icecc_generator

        icecc_string = "$( $ICECC_GENERATOR $)"
        env["CCCOM"] = " ".join([icecc_string, env["CCCOM"]])
        env["CXXCOM"] = " ".join([icecc_string, env["CXXCOM"]])
        env["SHCCCOM"] = " ".join([icecc_string, env["SHCCCOM"]])
        env["SHCXXCOM"] = " ".join([icecc_string, env["SHCXXCOM"]])

    # Make common non-compile jobs flow through icerun so we don't
    # kill the local machine. It would be nice to plumb ICERUN in via
    # SPAWN or SHELL but it is too much. You end up running `icerun
    # icecc ...`, and icecream doesn't handle that. We could try to
    # filter and only apply icerun if icecc wasn't present but that
    # seems fragile. If you find your local machine being overrun by
    # jobs, figure out what sort they are and extend this part of the
    # setup.
    icerun_commands = [
        "ARCOM",
        "LINKCOM",
        "PYTHON",
        "SHLINKCOM",
    ]

    for command in icerun_commands:
        if command in env:
            env[command] = " ".join(["$( $ICERUN $)", env[command]])

    # Uncomment these to debug your icecc integration
    if env['ICECREAM_DEBUG']:
        env['ENV']['ICECC_DEBUG'] = 'debug'
        env['ENV']['ICECC_LOGFILE'] = 'icecc.log'


def exists(env):
    if not env.subst("$ICECC"):
        return False

    icecc = env.WhereIs("$ICECC")
    if not icecc:
        # TODO: We should not be printing here because we don't always know the
        # use case for loading this tool. It may be that the user desires
        # writing this output to a log file or not even displaying it at all.
        # We should instead be invoking a callback to SConstruct that it can
        # interpret as needed. Or better yet, we should use some SCons logging
        # and error API, if and when one should emerge.
        print(f"Error: icecc not found at {env['ICECC']}")
        return False

    if 'ICECREAM_VERSION' in env and env['ICECREAM_VERSION'] >= _icecream_version_min:
        return True

    pipe = SCons.Action._subproc(
        env,
        SCons.Util.CLVar(icecc) + ["--version"],
        stdin="devnull",
        stderr="devnull",
        stdout=subprocess.PIPE,
    )

    if pipe.wait() != 0:
        print(f"Error: failed to execute '{env['ICECC']}'")
        return False

    validated = False

    if "ICERUN" in env:
        # Absoluteify, for parity with ICECC
        icerun = env.WhereIs("$ICERUN")
    else:
        icerun = env.File("$ICECC").File("icerun")
    if not icerun:
        print(f"Error: the icerun wrapper does not exist at {icerun} as expected")

    if "ICECC_CREATE_ENV" in env:
        icecc_create_env_bin = env.WhereIs("$ICECC_CREATE_ENV")
    else:
        icecc_create_env_bin = env.File("ICECC").File("icecc-create-env")
    if not icecc_create_env_bin:
        print(f"Error: the icecc-create-env utility does not exist at {icecc_create_env_bin} as expected")

    for line in pipe.stdout:
        line = line.decode("utf-8")
        if validated:
            continue  # consume all data
        version_banner = re.search(r"^ICECC ", line)
        if not version_banner:
            continue
        icecc_version = re.split("ICECC (.+)", line)
        if len(icecc_version) < 2:
            continue
        icecc_version = parse_version(icecc_version[1])
        if icecc_version >= _icecream_version_min:
            validated = True

    if validated:
        env['ICECREAM_VERSION'] = icecc_version
    else:
        print(f"Error: failed to verify icecream version >= {_icecream_version_min}, found {icecc_version}")

    return validated
