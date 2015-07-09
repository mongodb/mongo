SpiderMonkey in-tree documentation
==================================

This directory contains documentation for selected parts of SpiderMonkey. The
docs are published on the [Mozilla Developer Network][mdn], but this is the
authoritative copy; the MDN pages are updated from these files by a script. At
the moment, we have:

- `js/src/doc/Debugger`, SpiderMonkey's JavaScript debugging API, commonly
  known as `Debugger`.

- `js/src/doc/SavedFrame`, SpiderMonkey's compact representation for captured
  call stacks.

and that's it.

To format the documentation, you'll need to install [Pandoc][], a
documentation format conversion program. Pandoc can convert Markdown text,
which is pleasant to read and write, into HTML, which is what MDN expects.

[mdn]: http://developer.mozilla.org "Mozilla Developer Network"
[Pandoc]: http://www.johnmacfarlane.net/pandoc/installing.html

Management scripts
------------------

There are two scripts you may want to use while working with `js/src/doc`
subdirectories:

- `format.sh [--mdn] SOURCEDIR OUTPUTDIR` produces HTML from the document
  sources in SOURCEDIR, placing the results in OUTPUTDIR. You can then
  check their appearance with a web browser.

  Normally, format.sh arranges the links and HTML metadata so that the
  pages view correctly when visited at `file://` URLS pointing into
  OUTPUTDIR. However, pages published to MDN should not have the HTML
  metadata that stand-alone pages need, and their relative positions may be
  different; passing the `--mdn` switch tells `format.sh` to produce output
  for publication to MDN, not for previewing on disk.

  (Why are the links affected by `--mdn`? The MDN wiki allows you to create
  a page named `.../foo`, and then create sub-pages named `.../foo/bar`.
  This is a nice way to arrange, say, a summary page and then sub-pages
  providing details. But it's impossible to create the parallel structure
  on a POSIX file system: `.../foo` can't be both a file and a directory,
  so the links that would be correct when published on the wiki cannot be
  correct when previewing those pages on disk. Since OUTPUTDIR's layout
  can't match that of the wiki, we make it match that of SOURCEDIR.)

- `publish.sh SOURCEDIR OUTPUTDIR KEYID SECRET` calls `format.sh`, and then
  posts the pages to MDN, using KEYID and SECRET to establish your
  identity. This posts only changed pages, so you can run it whenever the
  text you have is the right stuff to publish, without creating gratuitous
  churn in the MDN page history.

  To generate KEYID and SECRET, visit the [MDN API key generation page][mdnkey].

[mdnkey]: https://developer.mozilla.org/en-US/keys/ "MDN API key generation"


Why not make the wiki the authoritative copy?
---------------------------------------------

Storing documentation in the same tree as the sources it describes has several
advantages:

- It's easy to handle documentation changes as part of the same review process
  we use for code changes. A patch posted to Bugzilla can contain code, test,
  and doc changes, all of which can be discussed together.

- The version control system that manages the code also manages its
  documentation. Branching the code (for Nightly, Aurora, Beta, or Release)
  branches the docs as well, so one can always find the docs that match the
  code in a given release stage.

- Documentation for proposed changes has a natural home: in the patches
  that implement the change (or, at least, in a patch attached to the bug
  discussing the change). There's no need to include "(not yet
  implemented)" markers in the published docs.


Subdirectory layout and script interface
----------------------------------------

Alongside the documentation source files, the SOURCEDIR passed to
`format.sh` should contain a file named `config.sh` describing the
directory's contents, how to format them, and where they should be
installed. This data is represented as executable shell code; `format.sh`
and `publish.sh` run the subdirectory's `config.sh` script to learn what
they should do.

The only effect of running a `SOURCEDIR/config.sh` script should be to
invoke the following commands:

`base-url BASE`
:   Treat BASE as the common prefix for some URLs appearing as arguments to
    subsequently executed commands (other than resource files). In
    describing the other commands, we use the metavariable RELATIVE-URL for
    URLs that are relative to BASE.

    This command should appear before those taking RELATIVE-URL arguments.

    BASE is treated as the root directory of the tree of formatted files.
    If OUTPUTDIR is the output directory passed to `format.sh` or
    `publish.sh`, then formatted files appear in OUTPUTDIR at the paths
    they would appear on MDN relative to BASE.

`markdown FILE RELATIVE-URL`
:   Treat FILE as Markdown source for a documentation page to be published
    at RELATIVE-URL.

`label LABEL [FRAGMENT] [TITLE]`
:   Define a label for use in Markdown reference-style links referring to
    the file given in the preceding `markdown` command. If the second
    argument begins with a `#` character, it is taken to be an HTML
    fragment identifier; the link refers to that fragment in the page.
    TITLE, if present, is the title for the link.

    For example:

        base-url https://devmo/Tools/
        markdown Conventions.md Debugger-API/Conventions
        label conventions "Debugger API: general conventions"
        label cv #completion-values "Debugger API: completion values"

    would mean that `Conventions.md` should be processed to create
    `https://devmo/Tools/Debugger-API/Conventions`; that Markdown files can
    refer to that page like this:

        ... follows some [common conventions][conventions] which ...

    and to the `#completion-values` fragment in that page like this:

        ... is passed a [completion value][cv] indicating ...

`absolute-label LABEL URL [TITLE]`
:   For reference-style links in this directory's Markdown files, define
    LABEL to refer to URL, an absolute URL. TITLE is an optional link title.

`resource LABEL FILE URL`
:   Treat FILE as a resource file (an image, for example) that should
    appear at URL. Since MDN likes to place "attachments" like images under
    different URL prefixes than the wiki pages themselves, URL is not
    relative to the BASE passed to base-url.

    LABEL can be the empty string if no Markdown documents need to refer to
    this resource file. (For example, the Markdown might use an SVG file,
    which in turn use the resource file.)

    Unfortunately, `publish.sh` can't automatically post these resources to
    MDN at the moment. However, it will check if the contents have changed,
    and print a warning.


This ought to be integrated with mach
-------------------------------------

Indeed. It should somehow be unified with 'mach build-docs', which seems to
format in-tree docs of a different kind, and use a different markup
language (ReStructuredText) and a different formatter (Sphinx).
