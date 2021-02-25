#!/bin/bash

set_goenv() {
    # Error out if not in the same directory as this script
    if [ ! -f ./set_goenv.sh ]; then
        echo "Must be run from mongo-tools top-level directory. Aborting."
        return 1
    fi

    # Set OS-level default Go configuration
    UNAME_S=$(PATH="/usr/bin:/bin" uname -s)
    case $UNAME_S in
        CYGWIN*)
            PREF_GOROOT="c:/golang/go1.12"
            PREF_PATH="/cygdrive/c/golang/go1.12/bin:/cygdrive/c/mingw-w64/x86_64-4.9.1-posix-seh-rt_v3-rev1/mingw64/bin:$PATH"
        ;;
        *)
            PREF_GOROOT="/opt/golang/go1.12"
            # XXX might not need mongodbtoolchain anymore
            PREF_PATH="$PREF_GOROOT/bin:/opt/mongodbtoolchain/v3/bin/:$PATH"
        ;;
    esac

    # Set OS-level compilation flags
    case $UNAME_S in
        'CYGWIN*')
            export CGO_CFLAGS="-D_WIN32_WINNT=0x0601 -DNTDDI_VERSION=0x06010000"
            ;;
        'Darwin')
            export CGO_CFLAGS="-mmacosx-version-min=10.11"
            export CGO_LDFLAGS="-mmacosx-version-min=10.11"
            ;;
    esac

    # XXX Setting the compiler might not be necessary anymore now that we're
    # using standard Go toolchain and if we don't put mongodbtoolchain into the
    # path.  But if we need to keep mongodbtoolchain for other tools (eg. python),
    # then this is probably still necessary to find the right gcc.
    if [ -z "$CC" ]; then
        UNAME_M=$(PATH="/usr/bin:/bin" uname -m)
        case $UNAME_M in
            aarch64)
                export CC=/opt/mongodbtoolchain/v3/bin/aarch64-mongodb-linux-gcc
            ;;
            ppc64le)
                export CC=/opt/mongodbtoolchain/v3/bin/ppc64le-mongodb-linux-gcc
            ;;
            s390x)
                export CC=/opt/mongodbtoolchain/v3/bin/s390x-mongodb-linux-gcc
            ;;
            *)
                # Not needed for other architectures
            ;;
        esac
    fi

    # If GOROOT is not set by the user, configure our preferred Go version and
    # associated path if available or error.
    if [ -z "$GOROOT" ]; then
        if [ -d "$PREF_GOROOT" ]; then
            export GOROOT="$PREF_GOROOT";
            export PATH="$PREF_PATH";
        else
            echo "GOROOT not set and preferred GOROOT '$PREF_GOROOT' doesn't exist. Aborting."
            return 1
        fi
    fi

    # Derive GOPATH from current directory, but error if the current diretory
    # doesn't look like a GOPATH structure.
    if expr "$(pwd)" : '.*src/github.com/mongodb/mongo-tools$' > /dev/null; then
        export GOPATH=$(echo $(pwd) | perl -pe 's{src/github.com/mongodb/mongo-tools}{}')
        if expr "$UNAME_S" : 'CYGWIN' > /dev/null; then
            export GOPATH=$(cygpath -w "$GOPATH")
        fi
    else
        echo "Current path '$(pwd)' doesn't resemble a GOPATH-style path. Aborting.";
        return 1
    fi

    return
}

print_ldflags() {
    VersionStr="$(git describe)"
    Gitspec="$(git rev-parse HEAD)"
    importpath="github.com/mongodb/mongo-tools/common/options"
    echo "-X ${importpath}.VersionStr=${VersionStr} -X ${importpath}.Gitspec=${Gitspec}"
}

print_tags() {
    tags=""
    if [ ! -z "$1" ]
    then
            tags="$@"
    fi
    UNAME_S=$(PATH="/usr/bin:/bin" uname -s)
    case $UNAME_S in
        Darwin)
            if expr "$tags" : '.*ssl' > /dev/null ; then
                tags="$tags openssl_pre_1.0"
            fi
        ;;
    esac
    echo "$tags"
}

# On linux, we want to set buildmode=pie for ASLR support
buildflags() {
    flags=""
    UNAME_S=$(PATH="/usr/bin:/bin" uname -s)
    case $UNAME_S in
        Linux)
            flags="-buildmode=pie"
        ;;
    esac
    echo "$flags"
}
