all: index

download:
	-@rm -f tzdata201*tar.gz
	-@rm -f tzcode201*tar.gz
	@echo "Downloading latest Olson TZDB release..."
	$(eval VERSION := $(shell curl -s -o - https://www.iana.org/time-zones | grep \"version\" | sed 's/.*version">//' | sed 's/<\/span.*//'))
	curl -s -o tzdata$(VERSION).tar.gz https://www.iana.org/time-zones/repository/releases/tzdata$(VERSION).tar.gz
	curl -s -o tzcode$(VERSION).tar.gz https://www.iana.org/time-zones/repository/releases/tzcode$(VERSION).tar.gz
	@echo " done"

clean: release-php-clean
	-rm -rf code
	-rm -f timezonedb.idx.php timezonedb.dta timezonedb.idx version-info.txt timezonedb-201*.tgz timezonedb.tgz timezonedb*.zip

tzdb: download
	-@rm -rf code
	@mkdir code
	@echo "Unpacking Olson TZDB release..."
	tar -C code -xzf tzdata*tar.gz
	tar -C code -xzf tzcode*tar.gz

code/zone.tab: tzdb
	@make -C code
	@echo "Compiling tzdata files..."
	@for i in africa antarctica australasia asia etcetera europe factory northamerica pacificnew southamerica backward; do \
		cat code/$$i | ./code/zic -d code/data -; \
		echo "- $$i"; \
	done;

timezonedb.idx.php: code/zone.tab
	php build-idx.php

timezonedb.dta: code/zone.tab
	php build-idx.php

timezonedb.idx: timezonedb.idx.php timezonedb.dta

version-info.txt: download
	find . -name tzdata*.tar.gz | sed 's/.*20/20/' | sed 's/\.tar.*//' > version-info.txt
	find . -name tzdata*.tar.gz | sed -E "s/^.*(20[0-9]{2}).*$$/\1/" | tr -d '\n' >> version-info.txt
	echo -n "." >> version-info.txt
	find . -name tzdata*.tar.gz | sed -E "s/^.*20[0-9]{2}(\w).*$$/\1/" | sed "s/\\n//" | od -A n -t d1 -N 1 | awk '{printf "%s", $$1 - 96}' >> version-info.txt

timezonedb.zip: code/zone.tab version-info.txt
	@echo -n "Making archive..."
	-@mkdir code/timezonedb-$(VERSION); cp -r code/data/. code/timezonedb-$(VERSION)/; cp version-info.txt code/timezonedb-$(VERSION)/
	-@cd code; zip --quiet -r ../timezonedb.zip timezonedb-$(VERSION)
	@cp timezonedb.zip timezonedb-$(VERSION).zip
	@echo " done"

timezonedb.h: timezonedb.idx version-info.txt create_dot_h_file.php
	php create_dot_h_file.php
	cp timezonedb.h ..

index: timezonedb.h

timezonedb.tgz: index
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	cp timezonedb.h ~/dev/php/pecl/timezonedb/trunk
	php update-package-xml.php ~/dev/php/pecl/timezonedb/trunk
	php update-package-version.php ~/dev/php/pecl/timezonedb/trunk
	pecl package ~/dev/php/pecl/timezonedb/trunk/package.xml
	cp timezonedb-$(VERSION).tgz timezonedb.tgz

release-pecl: timezonedb.tgz
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	$(eval TAG := $(shell cat version-info.txt | tail -n 1 | tr "." "_"))
	cd ~/dev/php/pecl/timezonedb/trunk; svn commit -m "Updated to version $(VERSION) ($(TZVERSION))" timezonedb.c timezonedb.h package.xml
	svn cp -m "Released version $(VERSION) ($(TZVERSION))" https://svn.php.net/repository/pecl/timezonedb/trunk https://svn.php.net/repository/pecl/timezonedb/tags/RELEASE_$(TAG)

release-docs: timezonedb.tgz
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	pecl upgrade -f timezonedb.tgz
	cd ~/dev/php/phpdoc/en/reference/datetime/; svn up
	cd ~/dev/php/phpdoc/en/reference/datetime/; php -dextension=timezonedb.so ../../../doc-base/scripts/gen-phpdoc-tz-list.php > timezones.xml
	cd ~/dev/php/phpdoc/en/reference/datetime/; svn commit -m "Updated to version $(VERSION) ($(TZVERSION))" timezones.xml

release-php-clean:
	-rm -rf /tmp/tz-tmp

release-php-clone: release-php-clean
	git clone git@git.php.net:php-src.git /tmp/tz-tmp

release-php-commit-70: timezonedb.tgz
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout PHP-7.0
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit-71: timezonedb.tgz release-php-commit-70
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout PHP-7.1
	cd /tmp/tz-tmp; git merge PHP-7.0 --strategy=ours -m "Empty merge"
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit-72: timezonedb.tgz release-php-commit-71
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout PHP-7.2
	cd /tmp/tz-tmp; git merge PHP-7.1 --strategy=ours -m "Empty merge"
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit-master: timezonedb.tgz release-php-commit-72
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout master
	cd /tmp/tz-tmp; git merge PHP-7.2 --strategy=ours -m "Empty merge"
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit: release-php-clone release-php-commit-70 release-php-commit-71 release-php-commit-master

release-php-push: release-php-commit
	cd /tmp/tz-tmp; git push origin PHP-7.0 PHP-7.1 PHP-7.2 master

release-php: release-php-clean release-php-clone release-php-commit release-php-push
