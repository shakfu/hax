# Build/test/lint entry points for humans, editors (nvim :make), and coding
# agents. Delegates to scripts/check.sh, which keeps successful output compact
# and keeps failure output focused on diagnostics.

BUILD_DIR ?= build

.PHONY: all tests lint bindings wheel install symlink clean

all:
	@BUILD_DIR=$(BUILD_DIR) scripts/check.sh build

tests:
	@BUILD_DIR=$(BUILD_DIR) scripts/check.sh test

lint:
	@scripts/check.sh lint

# The Python binding needs libhax, so it lives in its own build directory
# rather than turning -Dembed=true on for every build.
bindings:
	@BUILD_DIR=build-embed scripts/check.sh build

# meson-python drives the same meson build and packages its install output, so
# the wheel carries the cffi extension, libhax, and the hax binary.
#
# The deployment target has to reach the build as an environment variable rather than a meson
# option: it sets the compiler's minimum OS *and* is the only thing meson-python reads to compute
# the wheel's platform tag, which otherwise defaults to the version of macOS doing the building
# and produces a wheel pip rejects everywhere older. 11.0 is the floor for arm64. Ignored off
# macOS, and overridable from the environment.
MACOSX_DEPLOYMENT_TARGET ?= 11.0
wheel: export MACOSX_DEPLOYMENT_TARGET := $(MACOSX_DEPLOYMENT_TARGET)
wheel:
	uv build --wheel

install: all
	meson install -C $(BUILD_DIR)

# hax resolves subagent `hax` invocations through PATH, so development is nicest
# with the dev binary linked there; the symlink tracks every rebuild.
symlink: all
	@mkdir -p "$(HOME)/.local/bin"; \
	target="$$(cd "$(BUILD_DIR)" && pwd)/hax"; \
	link="$(HOME)/.local/bin/hax"; \
	ln -sf "$$target" "$$link" && echo "$$link -> $$target"

clean:
	rm -rf $(BUILD_DIR) dist
