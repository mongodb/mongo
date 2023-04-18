Building the timezone files
---------------------------

The building of files is done through the ``Makefile`` with:

- ``make clean`` (important if data files have changed)
- ``make release-pecl`` (PECL timezonedb extension)
- ``make release-docs`` (documentation updates)
- ``make release-php`` (changes to embed database in PHP)

It has the following prerequisites:

- The directory contains **one** ``tzcode2014i.tar.gz`` file and **one**
  ``tzdata2014i.tar.gz`` file.
- You have a PECL SVN checkout in ``~/dev/php/pecl``
- You have a PHPDOC GIT checkout in ``~/dev/php/phpdoc``
- You can commit to PHP's GIT ``php-src`` repository.
- You can commit to PHP's GIT ``doc-base`` and ``doc-en`` repositories.
- You can commit to PECL's SVN repository.
- ``php`` is in your path.

Do not run this script, unless you're Derick Rethans.
