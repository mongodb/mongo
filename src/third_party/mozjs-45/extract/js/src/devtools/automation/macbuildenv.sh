# We will be sourcing mozconfig files, which end up calling mk_add_options and
# ac_add_options with various settings. We only need the variable settings they
# create along the way.
mk_add_options() {
  : do nothing
}
ac_add_options() {
  : do nothing
}

topsrcdir="$SOURCE"

if [ -n "$AUTOMATION" ]; then
    # Download clang and some other things from tooltool server.
    # This should be done before running mozconfig to make clang detection
    # there work properly.
    TT_SERVER=${TT_SERVER:-https://api.pub.build.mozilla.org/tooltool/}
    ( cd $SOURCE/..; \
      ./scripts/scripts/tooltool/tooltool_wrapper.sh \
          $SOURCE/browser/config/tooltool-manifests/macosx64/releng.manifest \
          $TT_SERVER \
          setup.sh \
          $TOOLTOOL_HOME/tooltool.py )
fi

# Setup CC and CXX variables
. $topsrcdir/build/macosx/mozconfig.common
