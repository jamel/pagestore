#!/bin/sh
# New C files are included before you stage them unless Git ignores them.
set -eu
cd "$(git rev-parse --show-toplevel)"
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
files=$(git ls-files --cached --others --exclude-standard -- '*.c' '*.h')
[ -n "$files" ] || exit 0
if [ "${1:-}" = "--fix" ]; then
    # shellcheck disable=SC2086
    $CLANG_FORMAT -i $files
else
    # shellcheck disable=SC2086
    $CLANG_FORMAT --dry-run --Werror $files
fi
