# See README.rst for copyright and licensing details.

PYTHON=python
SOURCES=$(shell find extras -name "*.py")

check:
	PYTHONPATH=$(PWD) $(PYTHON) -m testtools.run extras.tests.test_suite

TAGS: ${SOURCES}
	ctags -e -R extras/

tags: ${SOURCES}
	ctags -R extras/

clean:
	rm -f TAGS tags
	find extras -name "*.pyc" -exec rm '{}' \;

### Documentation ###

apidocs:
	# pydoctor emits deprecation warnings under Ubuntu 10.10 LTS
	PYTHONWARNINGS='ignore::DeprecationWarning' \
		pydoctor --make-html --add-package extras \
		--docformat=restructuredtext --project-name=extras \
		--project-url=https://launchpad.net/extras


.PHONY: apidocs
.PHONY: check clean
