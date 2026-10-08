.PHONY: all build test int-test check dist clean install fmt fmt-check lint lint-tidy lint-scan lint-cppcheck lint-cocci cocci cocci-apply

SPATCH ?= spatch

BUILDDIR ?= build
PREFIX ?= /usr
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
MESON_SETUP_ARGS = --prefix $(PREFIX) -Dbuild_version=$(VERSION)

all: build

build:
	test -d $(BUILDDIR) || meson setup $(BUILDDIR) $(MESON_SETUP_ARGS)
	meson compile -C $(BUILDDIR)

test: build
	meson test -C $(BUILDDIR)

int-test:
	./int/run_tests

check: fmt-check test lint

dist: build
	meson dist -C $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

fmt:
	clang-format -i src/*.c src/*.h

fmt-check:
	clang-format --dry-run --Werror src/*.c src/*.h

lint: lint-tidy lint-scan lint-cppcheck

lint-tidy: build
	run-clang-tidy -p $(BUILDDIR) -j$$(nproc) -quiet src/*.c

lint-scan: build
	ninja -C $(BUILDDIR) scan-build

lint-cppcheck: build
	mkdir -p $(BUILDDIR)/cppcheck
	cppcheck --enable=all --std=c11 \
		--suppress=missingIncludeSystem \
		--suppress=missingInclude \
		--suppress=normalCheckLevelMaxBranches \
		--suppress=checkersReport \
		--suppress=constParameterCallback \
		--cppcheck-build-dir=$(BUILDDIR)/cppcheck \
		-j$$(nproc) \
		-I src/ src/*.c

spatch = "$(SPATCH)" --very-quiet --no-includes $(1) --sp-file "$(2)" src/*.c
cocci  = $(foreach rule,$(wildcard coccinelle/*.cocci),$(call spatch,$(1),$(rule)) &&) true

cocci:
	@$(call cocci)

cocci-apply:
	@$(call cocci,--in-place)

lint-cocci:
	@output=$$($(call cocci)) && printf '%s\n' "$$output" && test -z "$$output"

install: clean build
	meson install -C $(BUILDDIR)
