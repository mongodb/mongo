=============
Cross-channel
=============

Firefox is localized with a process nick-named *cross-channel*. This document
explains both the general idea as well as some technical details of that
process. The gist of it is this:

    We use one localization for all release channels.

There's a number of upsides to that:

* Localizers maintain a single source of truth. Localizers can work on Nightly,
  while updating Beta, Developer Edition or even Release and ESR.
* Localizers can work on strings at their timing.
* Uplifting string changes has less of an impact on the localization toolchain,
  and their impact can be evaluated case by case.

So the problem at hand is to have one localization source
and use that to build 5 different versions of Firefox. The goal is for that
localization to be as complete as possible for each version. While we do
allow for partial localizations, we don't want to enforce partial translations
on any version.

The process to tackle these follows these steps:

* Create resource to localize, ``gecko-strings``.

  * Review updates to that resource in *quarantine*.
  * Expose a known good state of that resource to localizers.

* The actual localization work happens in Pontoon.
* Write localizations back to ``l10n-central``.
* Get localizations into the builds.

.. digraph:: full_tree

    graph [ rankdir=LR ];
    "m-c" -> "quarantine";
    "m-b" -> "quarantine";
    "m-r" -> "quarantine";
    "c-c" -> "quarantine";
    "c-b" -> "quarantine";
    "c-r" -> "quarantine";
    "quarantine" -> "gecko-strings";
    "gecko-strings" -> "Pontoon";
    "Pontoon" -> "l10n-central";
    "l10n-central" -> "Nightly";
    "l10n-central" -> "Beta";
    "l10n-central" -> "Firefox";
    "l10n-central" -> "Daily";
    "l10n-central" -> "Thunderbird";
    {
      rank=same;
      "quarantine";
      "gecko-strings";
    }

.. note::

   The concept behind the quarantine in the process above is to
   protect localizers from churn on strings that have technical
   problems. Examples like that could be missing localization notes
   or copy that should be improved.

The resource to localize is a Mercurial repository, unifying
all strings to localize for all covered products and branches. Each revision
of this repository holds all the strings for a particular point in time.

There's three aspects that we'll want to unify here.

#. Create a version history that allows the localization team
   to learn where strings in the generated repository are coming from.
#. Unify the content across different branches for a single app.
#. Unify different apps, coming from different repositories.

The last item is the easiest, as ``mozilla-*`` and ``comm-*`` don't share
code or history. Thus, they're converted individually to disjunct directories
and files in the target repository, and the Mercurial history of each is interleaved
in the target history. When parents are needed for one repository, they're
rebased over the commits for the other. 

.. toctree::
   :maxdepth: 1

   graph
   commits
   content
   repositories
