# setup.sh - Run the rooting analysis on SpiderMonkey. See `README.txt` for usage.
#
# This script is based on the wiki page:
# http://trac.wildfiregames.com/wiki/StaticRootingAnalysis

set -eu

BUILD_DIR="$PWD"
ANALYSIS_SCRIPTDIR="$(dirname $0)"
MOZILLA_SRCDIR="$(cd $ANALYSIS_SCRIPTDIR && (hg root || git rev-parse --show-toplevel))"


# Requirements
# ============
#
# Download and unpack the Sixgill plugin binaries.
# (`wget -c` skips the download if you've already got the file.)
#
# This insecurely downloads software over HTTP. Sorry.
#
# The alternative is building your own Sixgill. That can be a pain and you may
# need some patches to get it to work on your Linux distribution. Ask sfink for
# details.

mkdir -p downloads
(cd downloads && wget -c http://people.mozilla.org/~sfink/data/hazards-sixgill.tar.xz)
tar xf downloads/hazards-sixgill.tar.xz

# Download and unpack GCC binaries compatible with the Sixgill plugin.
(cd downloads && wget -c http://people.mozilla.org/~sfink/data/hazards-gcc4.7.tar.xz)
tar xf downloads/hazards-gcc4.7.tar.xz


# Generate raw data (.xdb files)
# ==============================
#
# The first step is to generate the .xdb files that contain the information
# needed by the analysis. This is done by compiling SpiderMonkey with the
# sixgill plugin enabled. The plugin creates .xdb files which the analysis
# consumes.

PATH=$BUILD_DIR/sixgill/usr/bin:$PATH
export PATH
GCCDIR=$BUILD_DIR/gcc/bin
export GCCDIR

# Create a SpiderMonkey build directory and run configure.
mkdir -p spidermonkey-analysis
(cd spidermonkey-analysis && \
    $MOZILLA_SRCDIR/js/src/configure --enable-optimize)

# Make SpiderMonkey.
$MOZILLA_SRCDIR/js/src/devtools/rootAnalysis/run_complete \
    --build-root=$BUILD_DIR/spidermonkey-analysis \
    --binaries=$BUILD_DIR/sixgill/usr/bin \
    --wrap-dir=$BUILD_DIR/sixgill/usr/libexec/sixgill/scripts/wrap_gcc \
    --buildcommand='make' \
    --foreground \
    --no-logs \
    .


# Run the analysis
# ================

# Build *another* copy of SpiderMonkey, using the system C++ compiler, without
# Sixgill. This is what we use to run the analysis. (We don't let you skip this
# step by setting a $JS environment variable or something, because you need
# ctypes. Relax and spin a build.  Get yourself a cup of tea.)
mkdir -p spidermonkey-opt
(cd spidermonkey-opt && \
    $MOZILLA_SRCDIR/js/src/configure --enable-optimize --enable-ctypes --enable-nspr-build && \
    make -j8)
JS="$BUILD_DIR/spidermonkey-opt/dist/bin/js"

# Write a config file used by analyze.py.
rm -f defaults.py
echo "objdir = '${BUILD_DIR}/spidermonkey-analysis'" >> defaults.py
echo "sixgill = '${BUILD_DIR}/sixgill/usr/libexec/sixgill'" >> defaults.py
echo "sixgill_bin = '${BUILD_DIR}/sixgill/usr/bin'" >> defaults.py
echo "js = '${JS}'" >> defaults.py
echo "analysis_scriptdir = '${ANALYSIS_SCRIPTDIR}'" >> defaults.py

# Run the script that runs the scripts that do the analysis.
python2.7 "${MOZILLA_SRCDIR}/js/src/devtools/rootAnalysis/analyze.py" -j 8 callgraph
