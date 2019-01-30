.PHONY: help update run expectations

help:
	@echo "Script to regenerate wasm test cases (JS and WPT) from the spec repository."
	@echo ""
	@echo "- a spec directory is needed under js/src/wasm (not checked in). It can be a"
	@echo "  symbolic link or a real directory; if it's not there, the Makefile will clone"
	@echo "  the repository from the sources."
	@echo "- 'make update' makes sure the spec wast interpreter is up to date, regenerates"
	@echo "  all the JS and WPT test cases, and put them in the right directories, then"
	@echo "  updates the WPT manifest, if needed."
	@echo "- 'MOZCONFIG=/path/to/bin/firefox make run' runs the WPT test cases and prints a"
	@echo "  summary of the failures in the console."
	@echo "- 'MOZCONFIG=/path/to/bin/firefox make expectations' runs the WPT test cases and"
	@echo "  updates the expectations (known failures)."
	@echo ""
	@echo "Choose a rule: update or expectations."

update:
	[ -d ./spec ] || git clone https://github.com/webassembly/spec ./spec
	(cd ./spec/interpreter && make)
	./spec/test/build.py \
		--js ../jit-test/tests/wasm/spec \
		--html ../../../testing/web-platform/mozilla/tests/wasm
	echo "|jit-test| test-also-wasm-baseline; include:wasm-testharness.js" > ../jit-test/tests/wasm/spec/directives.txt
	../../../mach wpt-manifest-update

run:
	@[ -z $(MOZCONFIG) ] && echo "You need to define the MOZCONFIG env variable first."
	@[ -z $(MOZCONFIG) ] || ../../../mach wpt /_mozilla/wasm

expectations:
	@[ -z $(MOZCONFIG) ] && echo "You need to define the MOZCONFIG env variable first." || true
	@[ -z $(MOZCONFIG) ] || ../../../mach wpt /_mozilla/wasm --log-raw /tmp/expectations.log || true
	@[ -z $(MOZCONFIG) ] || ../../../mach wpt-update /tmp/expectations.log --no-patch
