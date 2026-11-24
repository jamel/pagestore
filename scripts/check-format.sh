#!/usr/bin/env bash
# New C files are included before you stage them unless Git ignores them.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}

if [[ ${1:-} == --staged ]]; then
    git diff --cached --name-only -z --diff-filter=ACMR -- '*.c' '*.h' |
        while IFS= read -r -d '' file; do
            if ! git show ":$file" | "$CLANG_FORMAT" --assume-filename="$file" --dry-run --Werror; then
                echo "Run scripts/check-format.sh --fix, then re-stage." >&2
                exit 1
            fi
        done
    exit
fi

git ls-files --cached --others --exclude-standard -z -- '*.c' '*.h' | (
    files=()
    while IFS= read -r -d '' file; do
        files+=("./$file")
    done
    [[ ${files[0]+set} ]] || exit 0
    if [[ ${1:-} == --fix ]]; then
        exec "$CLANG_FORMAT" -i "${files[@]}"
    fi
    exec "$CLANG_FORMAT" --dry-run --Werror "${files[@]}"
)
