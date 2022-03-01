Commits and Metadata
====================

When creating the commit for a particular revision, we need to find the
revisions on the other branches of cross-channel to unify the created
content with.

To do so, the cross-channel algorithm keeps track of metadata associated with
a revision in the target repository. This metadata is stored in the commit
message:

.. code-block:: bash

   X-Channel-Repo: mozilla-central
   X-Channel-Converted-Revision: af4a1de0a11cb3afbb7e50bcdd0919f56c23959a
   X-Channel-Repo: releases/mozilla-beta
   X-Channel-Revision: 65fb3f6bce94f8696e1571c2d48104dbdc0b31e2
   X-Channel-Repo: releases/mozilla-release
   X-Channel-Revision: 1c5bf69f887359645f1c3df4de0d0e3caf957e59
   X-Channel-Repo: releases/mozilla-esr68
   X-Channel-Revision: 4cbbc30e1ebc3254ec74dc041aff128c81220507

This metadata is appended to the original commit message when committing.
For each branch in the cross-channel configuration we have the name and
a revision. The revision that's currently converted is explicitly highlighted
by the ``-Converted-`` marker. On hg.mozilla.org, those revisions are also
marked up as links, so one can navigate from the converted changeset to the
original patch.

When starting the update for an incremental graph from the previous section,
the metadata is read from the target repository, and the data for the
currently converted branch is updated for each commit. Each revision in
this metadata then goes into the algorithm to create the unified content.
