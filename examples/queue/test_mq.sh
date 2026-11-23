#!/bin/sh
# Runs each mq command, including replay of changes left only in the log.
# Usage: test_mq.sh path/to/mq
set -eu
mq=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mq.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
q=$tmp/q
tab=$(printf '\t')

fail() {
    echo "test_mq: $*" >&2
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

# Reads don't create a queue.
status 2 "$mq" "$q" readers
[ ! -e "$q" ] || fail "readers created $q"

"$mq" "$q" sub billing
"$mq" "$q" sub audit
expect "0
1
2" "$mq" "$q" push one two three

# A read without --ack leaves the position alone.
expect "0${tab}one
1${tab}two
2${tab}three" "$mq" "$q" read billing
expect "0${tab}one" "$mq" "$q" read billing --limit 1 --ack
expect "1${tab}two
2${tab}three" "$mq" "$q" read billing
"$mq" "$q" ack billing 2
expect "" "$mq" "$q" read billing
status 2 "$mq" "$q" ack billing 3
status 1 "$mq" "$q" read nobody
status 2 "$mq" "$q" sub billing
status 2 "$mq" "$q" frobnicate

# A reader added at the latest message sees only newer ones.
"$mq" "$q" sub late latest
expect 3 "$mq" "$q" push four
expect "3${tab}four" "$mq" "$q" read late
expect "billing${tab}3${tab}1
audit${tab}0${tab}4
late${tab}3${tab}1" "$mq" "$q" readers
"$mq" "$q" unsub late
status 1 "$mq" "$q" unsub late

# A load fills many data pages. A reader can start at any kept message.
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "message %06d\n", i }' >"$tmp/load.txt"
expect "loaded 20000 messages" "$mq" "$q" load "$tmp/load.txt"
"$mq" "$q" sub probe 12345
expect "12345${tab}message 012341" "$mq" "$q" read probe --limit 1
out=$("$mq" "$q" check)
case $out in
    "ok: 20004 messages in "*) ;;
    *) fail "check printed '$out'" ;;
esac

# Changes made without a checkpoint exist only in the log. The next open replays them.
"$mq" --no-checkpoint "$q" push crash-1 >/dev/null
printf 'crash-2\n' | "$mq" --no-checkpoint "$q" load - >/dev/null
"$mq" --no-checkpoint "$q" ack billing 20004
log=$("$mq" -v "$q" readers 2>&1 >/dev/null)
case $log in
    *"recovered "*" WAL records"*) ;;
    *) fail "no replay message in: $log" ;;
esac
expect "20005${tab}crash-2" "$mq" "$q" read billing
out=$("$mq" "$q" stats)
case $out in
    *"messages        20006"*) ;;
    *) fail "stats printed '$out'" ;;
esac

# A small cap drops the oldest messages, and readers hear what they missed.
c=$tmp/capped
"$mq" --cap 5 "$c" sub slow
"$mq" "$c" push a b c d e f g h >/dev/null
out=$("$mq" "$c" read slow 2>"$tmp/err") || fail "read slow exited with $?"
[ "$out" = "3${tab}d
4${tab}e
5${tab}f
6${tab}g
7${tab}h" ] || fail "read slow printed '$out'"
err=$(cat "$tmp/err")
[ "$err" = "mq: slow missed 3 messages" ] || fail "read slow said '$err'"
out=$("$mq" "$c" check)
case $out in
    "ok: 5 messages in "*) ;;
    *) fail "check printed '$out'" ;;
esac

# Producer and reader threads in one process.
b=$tmp/bench
out=$("$mq" "$b" bench --producers 3 --readers 2 --messages 3000 --batch 4)
case $out in
    *"bench-0 read 3000 messages, missed 0"*"bench-1 read 3000 messages, missed 0"*) ;;
    *) fail "bench printed '$out'" ;;
esac
"$mq" "$b" check >/dev/null
echo "test_mq: ok"
