.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

.. role:: python(code)
   :language: python


===========================
Fluent to Fluent Migrations
===========================

It’s possible to migrate existing Fluent messages using :python:`COPY_PATTERN`
in a migration recipe. Unlike migrations from legacy content, it’s not possible
to interpolate the text, only to copy existing content without changes.

Consider for example a patch modifying an existing message to move the original
value to a :js:`alt` attribute.

Original message:


.. code-block:: fluent

  about-logins-icon = Warning icon
      .title = Breached website


New message:


.. code-block:: fluent

  about-logins-breach-icon =
      .alt = Warning icon
      .title = Breached website


This type of changes requires a new message identifier, which in turn causes
existing translations to be lost. It’s possible to migrate the existing
translated content with:


.. code-block:: python

    from fluent.migrate import COPY_PATTERN

    ctx.add_transforms(
        "browser/browser/aboutLogins.ftl",
        "browser/browser/aboutLogins.ftl",
        transforms_from(
    """
    about-logins-breach-icon =
        .alt = {COPY_PATTERN(from_path, "about-logins-icon")}
        .title = {COPY_PATTERN(from_path, "about-logins-icon.title")}
    """,from_path="browser/browser/aboutLogins.ftl"),
    )


In this specific case, the destination and source files are the same. The dot
notation is used to access attributes: :js:`about-logins-icon.title` matches
the :js:`title` attribute of the message with identifier
:js:`about-logins-icon`, while :js:`about-logins-icon` alone matches the value
of the message.


.. warning::

  Using the message identifier in :python:`COPY_PATTERN` will not migrate the
  message as a whole, with all its attributes, only its value.
