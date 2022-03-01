=======================
Cross-channel VCS Graph
=======================

We opted to create a commit graph that reflects the original history.
We initially tried a number of different algorithms to create a linearized
history, but the results reflected the original attribution of content poorly.

To do so, we take the original graph, pick only the revisions that are of
interest to l10n files, and create content for each of those revisions. It's
then committed with the original commit message and commit metadata, while
amending the message with metadata about which original commits went into
the generated content.

If you're only interested in a high-level overview of cross-channel
localization, you may want to skip forward to :doc:`commits`.

The history of ``mozilla-central`` is
`rather involved <https://blog.mozilla.org/axel/2017/03/23/cant-you-graph-that-graph/>`_.
There are a few aspects to be aware of:

#. The graph has multiple roots.
#. Files move in and out of the source paths configured by project configs.
#. Project configs change to expose existing files with history.
#. Merge days work differently in ``mozilla-central`` and ``comm-central``.
#. Merge days don't have correct file change metadata.

The overall process to update the graph for a new revision ``REV``
in the target goes as follows.

#. Find the revisions that are currently covered in the target repository.
#. Find all revisions that are ancestors of ``REV``, but not ancestors of
   already converted revisions from the previous step.
#. For all new revisions, find the ones that touch l10n. Keep track of new files.
#. For all new files, track their revisions, including moves and deletions.
#. Skip known-bad revisions.
#. Create a graph of the relevant revisions.
#. Fix the graph.
#. Ensure there's a single head.
#. Find the right entry points for the fixed graph onto the target graph.

The first few steps are straightforward, but the actual graph needs
work.

Sparse Graph
------------

Let's start with an example. Common graphs in ``mozilla-central``
look like the following, with "green" nodes being the ones affecting
localization that we want to transform.

.. digraph:: full_tree

    graph [ rankdir=LR ];
    "red0" -> "green1" ;
    "red0" -> "red1" ;
    "red1" -> "merge-red1" ;
    "green1" -> "merge-red1" ;
    "merge-red1" -> "green2" ;
    "merge-red1" -> "red2" ;
    "red2" -> "merge-red2" ;
    "green2" -> "merge-red2" ;
    "merge-red2" -> "green3" ;
    "merge-red2" -> "red3" ;
    "green3" -> "merge-red3" ;
    "red3" -> "merge-red3" ;
    "merge-red3" -> "green4" ;
    "merge-red3" -> "red4" ;
    "red4" -> "merge-red4" ;
    "green4" -> "merge-red4" ;
    "merge-red4" -> "green5" ;
    "merge-red4" -> "red5" ;
    "red5" -> "green4.5" ;
    "green4.5" -> "merge-red5" ;
    "green5" -> "merge-red5" ;
    "green1" [ color=green ] ;
    "green2" [ color=green ] ;
    "green3" [ color=green ] ;
    "green4" [ color=green ] ;
    "green4.5" [ color=green ] ;
    "green5" [ color=green ] ;

What we'd expect to get would be a graph that just goes through our nodes,
in this case effectively a linear graph.

.. digraph:: target

    graph [ rankdir=LR ];
    "green1" -> "green2" ;
    "green2" -> "green3" ;
    "green3" -> "green4" ;
    "green4" -> "green5" ;
    "green4" -> "green4.5" ;

All Arcs
--------

Natively, Mercurial creates a graph that shows all paths from each node
to another that can be taken in the full graph, creating, in this case,
a graph that connects all nodes.

.. digraph:: mesh

    graph [ rankdir=LR ];
    "green1" -> "green2" ;
    "green1" -> "green3" ;
    "green2" -> "green3" ;
    "green3" -> "green4" ;
    "green1" -> "green4" ;
    "green2" -> "green4" ;
    "green3" -> "green5" ;
    "green1" -> "green5" ;
    "green4" -> "green5" ;
    "green2" -> "green5" ;
    "green3" -> "green4.5" ;
    "green1" -> "green4.5" ;
    "green4" -> "green4.5" ;
    "green2" -> "green4.5" ;

Removing Shortcuts
------------------

The code generating our target repository needs to strip that
graph down. This is the step 7 in our list above. To do this, we find
and remove shortcuts in the graph. I.e., the arc from ``green1`` to
``green3`` is a shortcut for the connection ``green1`` → ``green2`` → ``green3``.
The algorithm removes the shortcut arc from ``green1`` to ``green3``.
Applying this algorithm over the full graph yields a simplifed graph as follows.


.. digraph:: mesh

    graph [ rankdir=LR ];
    "green1" -> "green2" ;
    "green2" -> "green3" ;
    "green3" -> "green4" ;
    "green1" -> "green5" ;
    "green4" -> "green5" ;
    "green1" -> "green4.5" ;
    "green4" -> "green4.5" ;

This is almost the graph we're looking for. There are still two more arcs
which shortcut, each from the root to each head of our graph here. They're
pretty hard to find by extending the algorithm we used in the first
simplification. You need to check two, three, four, and five intermediate
nodes to discover them already in this example, and in practice, these
shortcuts can span much wider ranges. So to find these, we use a second
algorithm, starting with merge nodes, i.e., nodes with more than one parent.
For each merge node, we try to find a non-trivial path to the merge node from
any of its parents. If we find one, we drop the arc from that parent to
the merge node. This code is efficient at this point as we only have a much
smaller list of merge nodes, compared to the initial graph. And the code
can rely on the fact that the numeric IDs of Mercurial changesets of parents
are always smaller than the child. That allows to abort the search when the
algorithm walks "past" the given merge node.

This results in the expected sparse graph that we started out with.

Single Head
-----------

To have a single repository state that we can translate, we ensure that we
have a single head in the target repository. As you can see in our example,
that doesn't necessarily need to be in our sparse set of nodes. If that's the
case, we pick the oldest merge commit that is a descendant of all our heads.
Oldest in this case means the one with the lowest numeric ID. There's not
necessarily a unique choice for this, and given that the numeric IDs depend on
how you added remote changesets to your local clone, this choice might be
only unique to your local clone.

Entry Points
------------

Generally, the sparse graph here will be an incremental update to an
existing sparse graph. The increment can also have roots that are not
children of the known converted revisions in the target repository.

This used to be very frequent back when we had multiple integration repositories
like ``mozilla-inbound`` that merged into ``mozilla-central``. The string
changes that landed there were based on a much older version of ``mozilla-central``
than the last conversion.

To find the right parent in the target repository, there's an algorithm that
finds a node in the target that corresponds to a parent in the source. That's
run for each root of the incremental sparse graph.

Part of this algorithm is also to skip over ``comm-*`` changesets in the target
when searching for entry points for ``mozilla-*`` and vice versa. That way,
we linearize the history across ``comm`` and ``mozilla``.

Maximum Parent Count
--------------------

In Mercurial, a changeset can have one or two parents, but no more. There's no
octopus merges like in git. The sparse graphs can contain merge nodes with more
than two parents, though, so when we encounter those merge nodes, we ingest
additional merge commits. The algorithm for finding good candidates for this
corresponds to the algorithm we used when ensuring a single head on the update.

With a known state of the changeset to convert, we'll describe the context
with which the content is generated in the next section. 
