#!/bin/sh
# Runs each kv command, including replay of changes left only in the log.
# Usage: test_kv.sh path/to/kv
set -eu
kv=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/kv.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
db=$tmp/db
tab=$(printf '\t')

fail() {
    echo "test_kv: $*" >&2
    exit 1
}

# expect WANT COMMAND...: COMMAND must succeed and print exactly WANT.
expect() {
    want=$1
    shift
    got=$("$@") || fail "'$*' exited with $?"
    [ "$got" = "$want" ] || fail "'$*' printed '$got', want '$want'"
}

# status WANT COMMAND...: COMMAND must exit with status WANT.
status() {
    want=$1
    shift
    rv=0
    "$@" >/dev/null 2>&1 || rv=$?
    [ "$rv" -eq "$want" ] || fail "'$*' exited with $rv, want $want"
}

# Reads don't create a store.
status 2 "$kv" "$db" get a
[ ! -e "$db" ] || fail "get created $db"

"$kv" "$db" put user:1 alice
"$kv" "$db" put user:2 bob
"$kv" "$db" put user:10 carol
expect alice "$kv" "$db" get user:1
"$kv" "$db" put user:1 alicia
expect alicia "$kv" "$db" get user:1

expect "user:1${tab}alicia
user:10${tab}carol
user:2${tab}bob" "$kv" "$db" scan --prefix user:
expect "user:10${tab}carol" "$kv" "$db" scan user:10 user:2
expect "user:1${tab}alicia" "$kv" "$db" scan --limit 1

"$kv" "$db" del user:2
status 1 "$kv" "$db" get user:2
status 1 "$kv" "$db" del user:2
status 2 "$kv" "$db" scan --prefix a b
status 2 "$kv" "$db" frobnicate

# A bulk load builds a multi-level tree.
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "key%06d\tvalue %d\n", i, i }' >"$tmp/load.tsv"
expect "loaded 20000 entries" "$kv" "$db" load "$tmp/load.tsv"
expect "value 12345" "$kv" "$db" get key012345
expect "key019999${tab}value 19999" "$kv" "$db" scan --limit 1 key019999
out=$("$kv" "$db" check)
case $out in
    "ok: 20002 keys in "*) ;;
    *) fail "check printed '$out'" ;;
esac

# Changes made without a checkpoint exist only in the log. The next open replays them.
"$kv" --no-checkpoint "$db" put crash:1 survived
printf 'crash:2\talso survived\n' | "$kv" --no-checkpoint "$db" load - >/dev/null
"$kv" --no-checkpoint "$db" del key000000
log=$("$kv" -v "$db" check 2>&1 >/dev/null)
case $log in
    *"recovered "*" WAL records"*) ;;
    *) fail "no replay message in: $log" ;;
esac
expect survived "$kv" "$db" get crash:1
expect "also survived" "$kv" "$db" get crash:2
status 1 "$kv" "$db" get key000000

# A bad line stops a load but keeps the lines before it.
printf 'good\t1\nno tab here\nlater\t2\n' >"$tmp/bad.tsv"
status 2 "$kv" "$db" load "$tmp/bad.tsv"
expect 1 "$kv" "$db" get good
status 1 "$kv" "$db" get later

out=$("$kv" "$db" stats)
case $out in
    *"keys            20004"*) ;;
    *) fail "stats printed '$out'" ;;
esac
echo "test_kv: ok"
