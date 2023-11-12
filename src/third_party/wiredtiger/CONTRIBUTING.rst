Contributing to the WiredTiger project
======================================

Pull requests are always welcome, and the WiredTiger dev team appreciates any help the community can
give to help make WiredTiger better.

For more information about how to contribute, please read `the WiredTiger Wiki on GitHub`_.

.. _the WiredTiger Wiki on GitHub: https://github.com/wiredtiger/wiredtiger/wiki/Contributing-to-WiredTiger


Coding Style
============

A consistent style makes it easier for engineers to read the sources
and to move between different parts of the code base. A coding style
also specifies important information that should be provided in the
code, through naming conventions and commenting guidelines. Coding
conventions also make it easier for tools to parse the code.

A good code style helps programmers avoid bugs by specifying idioms
that are less error-prone.

WiredTiger has separate coding styles for C and C++. This is because
the C coding style contains guidelines that are not appropriate (and
in some cases are bad practices) in C++. WiredTiger itself is written
in C, while tools like cppsuite and workgen are written in C++.


WiredTiger C Coding Style
=========================

The WiredTiger C coding standard is loosely based on K&R indentation
and formatted using Clang-Format. Some specific points are listed
below. This is not an exhaustive list, however. If in doubt, find an
existing example in the source code and copy it.

* Use space characters rather than tabs
* Line re-indents are always 2 space characters
* Lines wrap at 100 characters, split after an operator
* All groups of things should be in alphabetical order where possible
  (local variable declarations, flags, stat fields, etc)
* *struct* variables declarations should appear before the ``WT_*``
  structure declarations in a function. ``WT_*`` structure declarations
  should be in alphabetical order e.g.::

    struct timeval start, end;
    WT_CKPT *ckpt;
    WT_CONNECTION_IMPL *conn;

* Comments should:

  * Describe intended functionality
  * Not reference variable names where possible
  * Not reference JIRA ticket numbers nor pull request numbers
  * Be fully formed sentences
  * Use C-style with ``/* ... */`` and not C++ style double-slash comments
  * Single-line comments should place the delimiters on the same line as the text

* Multi-line comments should place the delimiters on their own lines,
  and begin each line of text with an asterisk::

    /* This is a valid comment. */
    // This is not a valid comment.

    /*
     * This is a valid
     * multi-line comment.
     */
    // This is not a
    // valid multi-line comment.

* Lines that need to be wrapped should be split so successive lines
  are longer if possible. This applies to function signatures too
  (exceptions are possible here). If in doubt, Clang-Format will
  handle this for you.
* Functions used across multiple files begin with a ``__wt_`` prefix,
  where prefix is a sub-system identifier (e.g log or btree)
* Static functions should begin with an `__` prefix, where prefix is a
  sub-system identifier (e.g log or btree)
* In a function declaration, the return value should be on a separate
  line so that the function name is at the left margin as shown below::

    int
    __wt_square(int x)
    {
	return (x * x);
    }

* Names for function output parameters should end with "p" and should
  appear at the end of the argument list, as shown below::

    static inline void
    __ref_index_slot(WT_SESSION_IMPL *session, WT_REF *ref,
      WT_PAGE_INDEX **pindexp, uint32_t *slotp)

* If a function takes a return value pointer as an argument (e.g.,
  ``WT_FH **fhpp``) and the function always fills in the pointer on
  successful return, then the function should always set the pointer
  to NULL at the beginning of the function. This is simpler than
  requiring all callers to initialize the argument. It also ensures
  that the caller is never surprised by finding random data in the
  return value (or at least we drop core if we're not doing the
  correct error handling).
* Use descriptive variable and function names. Use all lowercase
  letters with a ``_`` separator.  Use standard names for common
  WiredTiger structures (``WT_SESSION`` and ``WT_CONNECTION`` are
  "wt_session" and "wt_conn", ``WT_SESSION_IMPL`` and
  ``WT_CONNECTION_IMPL`` are "session" and "conn)
* Variable declaration blocks should not contain initializers
* Variable initialisation should be in a block at the beginning of a
  function.
* Variable initializations should follow either alphabetical order or
  variable declaration order.
* For the cases where the initialization isn't required, but a
  compiler wants it, tag them with this comment
  ``/* -Werror=maybe-uninitialized */``
* Pointers are compared to ``NULL`` , so "``(p == NULL)``",
  not "``(p == 0)``" or "``(!p)``"
* Use ``for(;;)`` to create an infinite loop, rather than
  ``while(true)``
* When returning a value from a function, use parentheses around the
  return value: ``return (0)``;
* Single statement blocks in conditions and loops do not use braces
  unless required to avoid ambiguity.
* Use names from WiredTiger namespaces to avoid collisions with
  application code and system include files:

  * WiredTiger's public function names begin with the string
    wiredtiger. For example, wiredtiger_open.
  * WiredTiger's public #define and structure typedef declarations
    begin with the string ``WT_``. For example ``WT_ERR`` and
    ``WT_SESSION``.
  * WiredTiger's private function names begin with the string
    ``__wt_``. For example, ``__wt_cursor_set_key``.
* When there is code shared with fail/non-fail cases, use the
  following style::

        if (0) {
    err:
            <non-shared fail code>
        }
	<shared fail/non-fail code>
	return (ret);

* When there is no code shared with fail/non-fail cases, use the
  following style::

	<non-fail code>
	return (0);
    err:
	<fail code>
	return (ret);

Run the ``./s_all`` script once your coding is finished. It will
reformat your code to adhere to many parts of our coding standard. But
it does not check everything. No tool can, for example, determine
whether your function names are sufficiently descriptive.
