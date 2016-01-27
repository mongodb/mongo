PYTHONPATH:=$(shell pwd)/lib:${PYTHONPATH}
PYTHON ?= python

all: check

check:
	PYTHONPATH=$(PYTHONPATH) $(PYTHON) -m testtools.run \
	    testscenarios.test_suite

clean:
	find . -name '*.pyc' -print0 | xargs -0 rm -f

TAGS: lib/testscenarios/*.py lib/testscenarios/tests/*.py
	ctags -e -R lib/testscenarios/

tags: lib/testscenarios/*.py lib/testscenarios/tests/*.py
	ctags -R lib/testscenarios/

.PHONY: all check clean
