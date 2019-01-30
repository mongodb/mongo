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

# Setup CC and CXX variables
. $topsrcdir/build/macosx/mozconfig.common
