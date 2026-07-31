.PHONY: help build test release clean

help:
	@echo "make build    build plugin/bin/iina-decklink-helper"
	@echo "make test     run the native smoke tests (needs a prior build)"
	@echo "make release  build, sign, notarize, tag, and publish a release —"
	@echo "              bump plugin/Info.json's version and commit it first;"
	@echo "              see RELEASING.md for one-time setup"
	@echo "make clean    remove build output (not native/third_party's FFmpeg)"

build:
	scripts/build_native.sh

test: build
	scripts/test_native.sh

release:
	scripts/release_macos.sh
	scripts/gh_release_macos.sh

clean:
	rm -rf native/build plugin/bin dist
