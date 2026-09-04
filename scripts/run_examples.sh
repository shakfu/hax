#!/bin/sh
# Run every example under bindings/python and report which ones still work. The test suite covers
# the functions the examples expose; this covers their main(), which nothing else does.
#
# Mock by default: each example is paired with the fixture in scripts/mock/ that drives it, so a
# run needs no API key, costs nothing, and is deterministic. --provider runs the same examples
# against a live model instead, which costs money; the fan-out and the triage queue are narrowed
# there because a smoke run has no reason to pay for the whole corpus or the whole backlog.
#
# Usage: scripts/run_examples.sh [-v] [--provider NAME [--model ID]]
#        [BUILD_DIR=dir] scripts/run_examples.sh

set -eu

cd "$(dirname "$0")/.."

verbose=0
provider=
model=

while [ $# -gt 0 ]; do
    case $1 in
    -v | --verbose)
        verbose=1
        ;;
    --provider)
        [ $# -ge 2 ] || { printf 'error: --provider needs a name\n' >&2; exit 2; }
        provider=$2
        shift
        ;;
    --model)
        [ $# -ge 2 ] || { printf 'error: --model needs an id\n' >&2; exit 2; }
        model=$2
        shift
        ;;
    -h | --help)
        sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        printf 'error: unknown argument %s\n' "$1" >&2
        exit 2
        ;;
    esac
    shift
done

# The package finds a meson-built extension beside the source tree on its own; say so plainly
# rather than letting seven examples fail with the same ImportError.
BUILD_DIR=${BUILD_DIR:-build-embed}
if ! ls "$BUILD_DIR"/bindings/_hax_cffi* >/dev/null 2>&1; then
    printf 'error: no cffi extension in %s/bindings\n' "$BUILD_DIR" >&2
    printf '       meson setup %s -Dembed=true && meson compile -C %s\n' \
        "$BUILD_DIR" "$BUILD_DIR" >&2
    exit 1
fi
HAX_EXTENSION_DIR=$(cd "$BUILD_DIR/bindings" && pwd -P)
export HAX_EXTENSION_DIR

# The interpreter meson built the extension with, chosen the same way meson.build chooses it.
if [ -n "${PYTHON:-}" ]; then
    python=$PYTHON
elif [ -x .venv/bin/python ]; then
    python=.venv/bin/python
else
    python=python3
fi

# One entry per example: the fixture that drives it under the mock provider, the arguments it
# takes there, and the arguments it takes against a live model. A live column of "-" means the
# example hard-codes provider="mock" and cannot be pointed at anything else.
example_fixture() {
    case $1 in
    example) echo python_tool.txt ;;
    example_database) echo database_agent.txt ;;
    example_async) echo interrupt_stall.txt ;;
    example_fanout) echo read_document.txt ;;
    example_supervisor) echo delegate.txt ;;
    example_judge) echo hello.txt ;;
    example_shared_state) echo triage.txt ;;
    esac
}

mock_args() {
    case $1 in
    example | example_async) echo "" ;;
    example_judge) echo "--candidate mock --candidate mock --judge mock" ;;
    # One report and one worker: the fixture is a single claim-and-submit, so the queue drains
    # and the example's "everything triaged" exit status still means something here.
    example_shared_state) echo "--provider mock --workers 1 --reports 1" ;;
    *) echo "--provider mock" ;;
    esac
}

live_args() {
    selection="--provider $provider"
    [ -n "$model" ] && selection="$selection --model $model"
    candidate=$provider
    [ -n "$model" ] && candidate="$provider:$model"
    case $1 in
    example | example_async) echo "-" ;;
    example_judge) echo "--candidate $candidate --candidate $candidate --judge $candidate" ;;
    # Three reports, not nine: each one costs both workers a claim and a submit round trip, and
    # a smoke run proves the queue drains without paying for the whole backlog.
    example_shared_state) echo "$selection --workers 2 --reports 3" ;;
    example_fanout) echo "$selection --glob p*.md" ;;
    *) echo "$selection" ;;
    esac
}

EXAMPLES="example example_database example_async example_fanout example_supervisor \
example_judge example_shared_state"

# $args below is expanded unquoted to split it into arguments. Pathname expansion rides along
# with that, so disable it: --glob p*.md is a pattern for the example, not for this shell.
set -f

log=$(mktemp)
trap 'rm -f "$log"' EXIT INT TERM

if [ -n "$provider" ]; then
    printf 'running examples against %s\n' "$provider"
else
    printf 'running examples against the mock provider\n'
fi

passed=0
failed=0
skipped=0
failures=

for name in $EXAMPLES; do
    if [ -n "$provider" ]; then
        args=$(live_args "$name")
        unset HAX_MOCK_SCRIPT
        if [ "$args" = "-" ]; then
            printf '  SKIP %-22s hard-codes the mock provider\n' "$name"
            skipped=$((skipped + 1))
            continue
        fi
    else
        args=$(mock_args "$name")
        HAX_MOCK_SCRIPT="$PWD/scripts/mock/$(example_fixture "$name")"
        export HAX_MOCK_SCRIPT
    fi

    started=$(date +%s)
    # Unquoted on purpose: $args is an argument list, not one argument. See set -f above.
    # shellcheck disable=SC2086
    if [ "$verbose" = 1 ]; then
        printf '\n--- %s %s ---\n' "$name" "$args"
        if "$python" "bindings/python/$name.py" $args; then status=0; else status=$?; fi
    elif "$python" "bindings/python/$name.py" $args >"$log" 2>&1; then
        status=0
    else
        status=$?
    fi
    elapsed=$(($(date +%s) - started))

    if [ "$status" = 0 ]; then
        printf '  ok   %-22s %ss\n' "$name" "$elapsed"
        passed=$((passed + 1))
    else
        printf '  FAIL %-22s exit %s\n' "$name" "$status"
        [ "$verbose" = 1 ] || sed 's/^/       /' "$log"
        failed=$((failed + 1))
        failures="$failures $name"
    fi
done

printf '%s passed, %s failed, %s skipped\n' "$passed" "$failed" "$skipped"
if [ "$failed" != 0 ]; then
    printf 'failed:%s\n' "$failures" >&2
    exit 1
fi
