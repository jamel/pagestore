#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
IWYU=${IWYU:-include-what-you-use}
mode=${1:-}

if [[ $mode == --staged ]]; then
    git diff --cached --name-only -z --diff-filter=ACMR -- '*.c' '*.h'
else
    git ls-files --cached --others --exclude-standard -z -- '*.c' '*.h'
fi | (
    files=()
    while IFS= read -r -d '' file; do
        files+=("./$file")
    done
    [[ ${files[0]+set} ]] || exit 0

    if ! iwyu=$(command -v "$IWYU"); then
        echo "$IWYU not found. Install it or set IWYU." >&2
        exit 1
    fi
    [[ $iwyu == /* ]] || iwyu=$PWD/$iwyu

    if [[ $mode == --staged ]]; then
        # IWYU opens headers from disk, so give it the staged versions as well.
        staged=$(mktemp -d "${TMPDIR:-/tmp}/pagestore-iwyu.XXXXXXXX")
        [[ $staged == /* ]] || staged=$PWD/$staged
        trap 'rm -rf -- "$staged"' EXIT
        git checkout-index --all --ignore-skip-worktree-bits --prefix="$staged/"
        cd "$staged"
    fi

    # Match the project's C standard, include paths, and platform defines.
    flags=(-std=c11 -Iinclude -Isrc -Itests/support)
    case $(uname -s) in
        Linux) flags+=(-D_GNU_SOURCE) ;;
        Darwin) flags+=(-D_DARWIN_C_SOURCE) ;;
    esac

    iwyu_args=(-Xiwyu --error)
    if [[ $mode == --fix ]]; then
        fixer=${FIX_INCLUDES:-fix_includes.py}
        if [[ -z ${FIX_INCLUDES:-} ]] && ! command -v "$fixer" >/dev/null 2>&1; then
            fixer=fix_include
        fi
        if ! command -v "$fixer" >/dev/null 2>&1; then
            echo "$fixer not found. Install the IWYU fixer or set FIX_INCLUDES." >&2
            exit 1
        fi
        iwyu_args=(-Xiwyu --error=0 -Xiwyu --verbose=1)
    fi

    status=0
    reports=()
    for file in "${files[@]}"; do
        if ! output=$("$iwyu" "${flags[@]}" "${iwyu_args[@]}" "$file" 2>&1); then
            printf '%s\n' "$output" >&2
            status=1
        elif [[ $mode == --fix ]]; then
            reports+=("$output")
        fi
    done
    if [[ $mode == --fix && $status == 0 ]]; then
        # Apply the reports together because they refer to the original line numbers.
        printf '%s\n' "${reports[@]}" | "$fixer" --nosafe_headers --basedir "$PWD" -- "${files[@]}"
    fi
    exit "$status"
)
