#!/bin/bash -E

# Pre-checks for validation and linting
#
# These checks do not provide a fix and are quicker to run,
# allowing CI to fail quickly on basic linting or validation errors

FAILED=()
CURRENT=""

read -ra BAZEL_BUILD_OPTIONS <<< "${BAZEL_BUILD_OPTIONS:-}"


trap_errors () {
    local frame=0 command line sub file
    if [[ -n "$CURRENT" ]]; then
        command=" (${CURRENT})"
    fi
    set +v
    while read -r line sub file < <(caller "$frame"); do
        if [[ "$frame" -ne "0" ]]; then
            FAILED+=("  > ${sub}@ ${file} :${line}")
        else
            FAILED+=("${sub}@ ${file} :${line}${command}")
        fi
        ((frame++))
    done
    set -v
}

trap trap_errors ERR
trap exit 1 INT

CURRENT=shellcheck
./tools/code_format/check_shellcheck_format.sh check

CURRENT=configs
bazel run "${BAZEL_BUILD_OPTIONS[@]}" //configs:example_configs_validation

CURRENT=flake8
bazel run "${BAZEL_BUILD_OPTIONS[@]}" //tools/code_format:python_flake8 "$(pwd)"

if [[ "${#FAILED[@]}" -ne "0" ]]; then
    echo "TESTS FAILED:" >&2
    for failed in "${FAILED[@]}"; do
        echo "  $failed" >&2
    done
    exit 1
fi
