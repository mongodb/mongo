BSON_PKGS = $(shell etc/list_pkgs.sh ./bson)
BSON_TEST_PKGS = $(shell etc/list_test_pkgs.sh ./bson)
MONGO_PKGS = $(shell etc/list_pkgs.sh ./mongo)
MONGO_TEST_PKGS = $(shell etc/list_test_pkgs.sh ./mongo)
UNSTABLE_PKGS = $(shell etc/list_pkgs.sh ./x)
UNSTABLE_TEST_PKGS = $(shell etc/list_test_pkgs.sh ./x)
TAG_PKG = $(shell etc/list_pkgs.sh ./tag)
TAG_TEST_PKG = $(shell etc/list_test_pkgs.sh ./tag)
EXAMPLES_PKGS = $(shell etc/list_pkgs.sh ./examples)
EXAMPLES_TEST_PKGS = $(shell etc/list_test_pkgs.sh ./examples)
PKGS = $(BSON_PKGS) $(MONGO_PKGS) $(UNSTABLE_PKGS) $(TAG_PKG) $(EXAMPLES_PKGS)
TEST_PKGS = $(BSON_TEST_PKGS) $(MONGO_TEST_PKGS) $(UNSTABLE_TEST_PKGS) $(TAG_PKG) $(EXAMPLES_TEST_PKGS)

TEST_TIMEOUT = 600

.PHONY: default
default: check-env check-fmt vet build-examples lint errcheck test-cover test-race

.PHONY: check-env
check-env:
	etc/check_env.sh

.PHONY: doc
doc:
	godoc -http=:6060 -index

.PHONY: build-examples
build-examples:
	go build $(BUILD_TAGS) ./examples/... ./x/network/examples/...

.PHONY: build
build:
	go build $(filter-out ./core/auth/internal/gssapi,$(PKGS))

.PHONY: check-fmt
check-fmt:
	@gofmt -l -s $(PKGS) | read; if [ $$? == 0 ]; then echo "gofmt check failed for:"; gofmt -l -s $(PKGS) | sed -e 's/^/ - /'; exit 1; fi

.PHONY: fmt
fmt:
	gofmt -l -s -w $(PKGS)

.PHONY: lint
lint:
	golint $(PKGS) | ./etc/lintscreen.pl .lint-whitelist

.PHONY: lint-add-whitelist
lint-add-whitelist:
	golint $(PKGS) | ./etc/lintscreen.pl -u .lint-whitelist
	sort .lint-whitelist -o .lint-whitelist

.PHONY: errcheck
errcheck:
	errcheck -exclude .errcheck-excludes ./bson/... ./mongo/... ./x/...

.PHONY: test
test:
	go test $(BUILD_TAGS) -timeout $(TEST_TIMEOUT)s $(TEST_PKGS)

.PHONY: test-cover
test-cover:
	go test $(BUILD_TAGS) -timeout $(TEST_TIMEOUT)s -cover $(COVER_ARGS) $(TEST_PKGS)

.PHONY: test-race
test-race:
	go test $(BUILD_TAGS) -timeout $(TEST_TIMEOUT)s -race $(TEST_PKGS)

.PHONY: test-short
test-short:
	go test $(BUILD_TAGS) -timeout $(TEST_TIMEOUT)s -short $(TEST_PKGS)

.PHONY: update-bson-corpus-tests
update-bson-corpus-tests:
	etc/update-spec-tests.sh bson-corpus

.PHONY: update-connection-string-tests
update-connection-string-tests:
	etc/update-spec-tests.sh connection-string

.PHONY: update-crud-tests
update-crud-tests:
	etc/update-spec-tests.sh crud

.PHONY: update-initial-dns-seedlist-discovery-tests
update-initial-dns-seedlist-discovery-tests:
	etc/update-spec-tests.sh initial-dns-seedlist-discovery

.PHONY: update-max-staleness-tests
update-max-staleness-tests:
	etc/update-spec-tests.sh max-staleness

.PHONY: update-server-discovery-and-monitoring-tests
update-server-discovery-and-monitoring-tests:
	etc/update-spec-tests.sh server-discovery-and-monitoring

.PHONY: update-server-selection-tests
update-server-selection-tests:
	etc/update-spec-tests.sh server-selection

.PHONY: update-notices
update-notices:
	etc/generate-notices.pl > THIRD-PARTY-NOTICES

.PHONY: vet
vet:
	go vet -cgocall=false -composites=false -unusedstringmethods="Error" $(PKGS)


# Evergreen specific targets
.PHONY: evg-test
evg-test:
	go test $(BUILD_TAGS) -v -timeout $(TEST_TIMEOUT)s $(TEST_PKGS) > test.suite

.PHONY: evg-test-auth
evg-test-auth:
	go run -tags gssapi ./x/network/examples/count/main.go -uri $(MONGODB_URI)

# benchmark specific targets and support
perf:driver-test-data.tar.gz
	tar -zxf $< $(if $(eq $(UNAME_S),Darwin),-s , --transform=s)/data/perf/
	@touch $@
driver-test-data.tar.gz:
	curl --retry 5 "https://s3.amazonaws.com/boxes.10gen.com/build/driver-test-data.tar.gz" -o driver-test-data.tar.gz --silent --max-time 120
benchmark:perf
	go test $(BUILD_TAGS) -benchmem -bench=. ./benchmark
driver-benchmark:perf
	@go run cmd/godriver-benchmark/main.go | tee perf.suite
.PHONY:benchmark driver-benchmark
