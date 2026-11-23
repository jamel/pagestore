# B+tree example

A B+tree of byte-string keys and values, built on PageStore's public API.
`kv` uses it to keep a key-value map in a directory, with crash
recovery from a write-ahead log.

| File | Contents |
| --- | --- |
| [`btree.h`](btree.h) | Tree API: create, put, delete, get, cursors, structure check |
| [`btree.c`](btree.c) | Implementation. Its opening comment shows the page layouts |
| [`kv.c`](kv.c) | `kv`, the command-line key-value tool |
| [`test_btree.c`](test_btree.c) | Tree tests against a sorted-array model |
| [`test_kv.sh`](test_kv.sh) | `kv` commands end to end, including log replay |

## Running it

Examples build only on request. `./dev.py build --example` builds them
with the library, and the tool ends up at `build/debug/examples/btree/kv`.
Add `--release` for an optimized build in `build/release`.

```sh
$ kv data put user:1 alice
$ kv data put user:2 bob
$ kv data get user:1
alice
$ kv data scan --prefix user:
user:1	alice
user:2	bob
$ awk 'BEGIN { for (i = 0; i < 100000; i++) printf "key%06d\tvalue %d\n", i, i }' |
    kv data load -
loaded 100000 entries
$ kv data stats
keys            100002
height          3
leaf pages      647, 98% full
inner pages     4, 67% full
...
$ kv data check
ok: 100002 keys in 652 pages
```

Run `kv --help` for every command and option.

To see recovery at work, skip the final checkpoint. The change then exists
only in the log, as after a crash, and the next open replays it:

```sh
$ kv --no-checkpoint data put late arrival
$ kv -v data get late
kv: store: WAL data/wal opened: ...
kv: store: recovered 1 WAL records (LSNs 100004..100004) above the durable LSN 100003
arrival
```

## How it fits on PageStore

- **Writes** run inside a store operation. `kv` begins the operation
  at the next LSN, changes the tree, appends a logical record (put or
  delete) to the WAL, flushes it and commits. An error aborts the
  operation, which discards every page the tree touched.
- **Recovery** replays the records above the last checkpoint through the
  same function that applied them live.
- **Reads** go through a snapshot, so readers on other threads see a
  consistent tree while the writer works. The tests check this with a
  reader thread.
- **Durability** comes from `ps_checkpoint` at the end of each run. The
  store flushes the log before writing pages, so the pages on disk are
  never ahead of the log.

The tree starts from its meta page, and the caller keeps that page's ID.
The store has no place to record it, so `kv` uses a fixed ID: page 1, the
first page a new store hands out (page 0 holds the allocator's bitmap).
`kv` checks that it got page 1 when it creates the tree. The tree itself
works with any ID.

## Design notes

- Nodes are slotted pages holding variable-length cells. With 4 KiB pages,
  keys can be up to 1006 bytes, and a key and value together up to 1008.
  Larger values would need overflow pages, which this example leaves out.
- Leaves link to their right sibling, so scans walk the leaf chain.
- Leaf splits pass up the shortest separator that divides the two halves.
- A run of ascending or descending inserts splits at the insert point, so
  sorted loads fill pages wherever they land. Random inserts split by bytes.
- Deletes merge a node that falls below a quarter full with a sibling, or
  borrow entries from it.

## Tests

```sh
./dev.py test --example --label example
```
