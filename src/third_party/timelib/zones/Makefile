all: index

download:
	-@rm -f tzdata20*tar.gz
	-@rm -f tzcode20*tar.gz
	@echo "Downloading latest Olson TZDB release..."
	$(eval VERSION := $(shell curl -s -o - https://www.iana.org/time-zones | grep \"version\" | sed 's/.*version">//' | sed 's/<\/span.*//'))
	curl -s -o tzdata$(VERSION).tar.gz https://data.iana.org/time-zones/releases/tzdata$(VERSION).tar.gz
	curl -s -o tzcode$(VERSION).tar.gz https://data.iana.org/time-zones/releases/tzcode$(VERSION).tar.gz
	@echo " done"

clean: release-php-clean
	-rm -rf code
	-rm -f timezonedb.idx.php timezonedb.dta timezonedb.idx version-info.txt timezonedb-20[12]*.tgz timezonedb.tgz timezonedb*.zip

tzdb: download
	-@rm -rf code
	@mkdir code
	@echo "Unpacking Olson TZDB release..."
	tar -C code -xzf tzdata*tar.gz
	tar -C code -xzf tzcode*tar.gz

code/zone.tab: tzdb
	@make PACKRATDATA=backzone PACKRATLIST=zone.tab -C code
	@echo "Compiling tzdata files..."
	cat code/main.zi | ./code/zic -b fat -d code/data -
	cat code/main.zi | ./code/zic -b slim -d code/data-slim -

version-info.txt: download
	find . -name tzdata*.tar.gz | sed 's/.*202/202/' | sed 's/\.tar.*//' > version-info.txt
	find . -name tzdata*.tar.gz | sed -E "s/^.*(20[0-9]{2}).*$$/\1/" | tr -d '\n' >> version-info.txt
	echo -n "." >> version-info.txt
	find . -name tzdata*.tar.gz | sed -E "s/^.*20[0-9]{2}(\w).*$$/\1/" | sed "s/\\n//" | od -A n -t d1 -N 1 | awk '{printf "%s", $$1 - 96}' >> version-info.txt

timezonedb.zip: code/zone.tab version-info.txt
	@echo -n "Making archive..."
	-@mkdir code/timezonedb-$(VERSION); cp -r code/data/. code/timezonedb-$(VERSION)/; cp version-info.txt code/timezonedb-$(VERSION)/
	-@cd code; zip --quiet -r ../timezonedb.zip timezonedb-$(VERSION)
	@cp timezonedb.zip timezonedb-$(VERSION).zip
	@echo " done"

timezonedb.h: code/zone.tab version-info.txt create-timezonedb.php
	php create-timezonedb.php
	cp timezonedb.h ..

index: timezonedb.h

timezonedb.tgz: index
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	cp timezonedb.h ~/dev/php/pecl-datetime-timezonedb
	php update-package-xml.php ~/dev/php/pecl-datetime-timezonedb
	php update-package-version.php ~/dev/php/pecl-datetime-timezonedb
	pecl package ~/dev/php/pecl-datetime-timezonedb/package.xml
	cp timezonedb-$(VERSION).tgz timezonedb.tgz

release-pecl: timezonedb.tgz
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	$(eval TAG := $(shell cat version-info.txt | tail -n 1 | tr "." "_"))
	cd ~/dev/php/pecl-datetime-timezonedb; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" timezonedb.c timezonedb.h package.xml php_timezonedb.h
	cd ~/dev/php/pecl-datetime-timezonedb; git tag -s RELEASE_$(TAG) -m "Go with $(VERSION)"
	cd ~/dev/php/pecl-datetime-timezonedb; git push origin master RELEASE_$(TAG)

release-docs: timezonedb.tgz
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	-pecl upgrade -f timezonedb.tgz
	cd ~/dev/php/phpdoc/en; git pull
	cd ~/dev/php/phpdoc/en/reference/datetime/; php -dextension=timezonedb.so ../../../doc-base/scripts/gen-phpdoc-tz-list.php > timezones.xml
	cd ~/dev/php/phpdoc/en/reference/datetime/; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" timezones.xml && git push

release-php-clean:
	-rm -rf /tmp/tz-tmp

release-php-clone: release-php-clean
	git clone git@github.com:/php/php-src /tmp/tz-tmp

release-php-commit-80: timezonedb.tgz
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout PHP-8.0
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit-81: timezonedb.tgz release-php-commit-80
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout PHP-8.1
	cd /tmp/tz-tmp; git merge PHP-8.0 --strategy=ours -m "Empty merge"
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit-82: timezonedb.tgz release-php-commit-81
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout PHP-8.2
	cd /tmp/tz-tmp; git merge PHP-8.1 --strategy=ours -m "Empty merge"
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit-master: timezonedb.tgz release-php-commit-82
	$(eval VERSION := $(shell cat version-info.txt | tail -n 1))
	$(eval TZVERSION := $(shell cat version-info.txt | head -n 1))
	cd /tmp/tz-tmp; git checkout master
	cd /tmp/tz-tmp; git merge PHP-8.2 --strategy=ours -m "Empty merge"
	cp timezonedb.h /tmp/tz-tmp/ext/date/lib
	cd /tmp/tz-tmp; git commit -m "Updated to version $(VERSION) ($(TZVERSION))" ext/date/lib/timezonedb.h

release-php-commit: release-php-clone release-php-commit-80 release-php-commit-81 release-php-commit-82 release-php-commit-master

release-php-push: release-php-commit
	cd /tmp/tz-tmp; git push origin PHP-8.0 PHP-8.1 PHP-8.2 master

release-php: release-php-clean release-php-clone release-php-commit release-php-push
