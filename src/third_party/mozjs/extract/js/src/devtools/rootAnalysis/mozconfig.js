# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This mozconfig is used when analyzing the source code of the js/src tree for
# GC rooting hazards. See
# <https://wiki.mozilla.org/Javascript:SpiderMonkey:ExactStackRooting>.

ac_add_options --enable-project=js

# Also compile NSPR to see through its part of the control flow graph (not
# currently needed, but also helps with weird problems finding the right
# headers.)
ac_add_options --enable-nspr-build

. $topsrcdir/js/src/devtools/rootAnalysis/mozconfig.common
