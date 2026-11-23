# Message queue example

A durable message queue built on PageStore's public API. Producer threads
append messages. Each named reader reads every message in order at its own
pace and acknowledges what it has handled. The queue keeps the newest `cap`
messages. A reader that falls further behind misses the older ones and is
told how many. `mq` is a command-line tool for the queue, with a benchmark
that runs producer and reader threads in one process.

| File | Contents |
| --- | --- |
| [`radix.h`](radix.h) | Radix array: maps dense u64 indexes to fixed-width values |
| [`radix.c`](radix.c) | Implementation. Its opening comment shows the node layout |
| [`msglog.h`](msglog.h) | Message log: messages by sequence number, the cap, reader positions |
| [`msglog.c`](msglog.c) | Implementation. Its opening comment shows the page layouts |
| [`mq.h`](mq.h) | Thread-safe queue: WAL records, recovery, group commit, blocking reads |
| [`mq.c`](mq.c) | Implementation. Its opening comment explains durability and locking |
| [`mqtool.c`](mqtool.c) | `mq`, the command-line tool |
| [`test_msglog.c`](test_msglog.c) | Radix array and log tests against models |
| [`test_mq.c`](test_mq.c) | API, threads, snapshot cut-off and power-loss tests |
| [`test_mq.sh`](test_mq.sh) | `mq` commands end to end, including log replay |

## Running it

Examples build only on request. `./dev.py build --example` builds them with
the library, and the tool ends up at `build/debug/examples/queue/mq`. Add
`--release` for an optimized build in `build/release`.

```sh
$ mq data sub billing
$ mq data push hello world
0
1
$ mq data read billing --ack
0	hello
1	world
$ mq data read billing          # acknowledged, so nothing new
$ mq data readers
billing	2	0
$ mq --cap 3 small sub slow
$ mq small push a b c d e >/dev/null
$ mq small read slow
mq: slow missed 2 messages
2	c
3	d
4	e
$ mq data bench --producers 16 --readers 2
pushed 20000 messages of 100 bytes in 2.94 s: 6797 messages/s, 0.6 MiB/s
bench-0 read 20000 messages, missed 0, done at 2.94 s
bench-1 read 20000 messages, missed 0, done at 2.94 s
```

A read without `--ack` leaves the reader's position alone, so the next read
shows the same messages again. Run `mq --help` for every command and option.

Every push waits for its WAL record to be durable, so throughput depends on
fsync speed and on how many pushes can share one. On a disk where fsync
takes about 1 ms, one producer gets about 1000 messages per second. Four
producers get about 2500, sixteen about 6800, and four producers pushing
16 messages at a time about 17600.

## Layers

```
mq.h       threads, WAL records, recovery, group commit, durable watermark
msglog.h   messages, cap and reader positions, inside the caller's operation
radix.h    u64 index -> value, inside the caller's operation
PageStore
```

The two lower layers work like the B+tree example: changes run inside an
operation the caller begins and commits, and reads go through a snapshot.
Neither knows about page IDs beyond what it is handed. `mq` uses a fixed
meta page ID: page 1, the first page a new store hands out. It checks that
it got page 1 when it creates the log.

## How it fits on PageStore

- **Messages** are packed end to end in one logical byte space made of data
  pages, so a message can span pages. One radix array maps a sequence
  number to the message's byte offset. Another maps a logical page number to
  its page ID, since the store hands out page IDs in no useful order. A
  message's length is the gap to the next message's offset.
- **Writes** run inside a store operation. `mq` takes its lock, begins the
  operation at the next LSN, applies a logical record (push, subscribe,
  unsubscribe or ack), appends it to the WAL and commits.
- **Group commit** happens after the lock is released. One waiting thread
  flushes the WAL through the newest committed LSN, and the others wait for
  it. A call returns once its record is durable.
- **Reads** take no lock. A reader's handle reads through a snapshot and
  sees only messages whose push is durable. That way no reader handles a
  message that a crash could take back. If the store cuts the snapshot off
  to free memory, the handle opens a new one and carries on.
- **Recovery** replays the records above the last checkpoint through the
  same function that applied them live.
- **Trimming** frees the data pages only the dropped messages used, along
  with radix array nodes that hold only dropped entries. A page allocated
  and freed between two checkpoints is never written to disk, so a queue
  that turns over quickly writes most messages only to the WAL.

## Design notes

- A radix array stores no keys, because each index has a fixed slot. Leaves
  fill completely and nothing splits or merges. With 4 KiB pages a leaf
  holds 507 offsets or 1014 page IDs, and an inner node 1014 children. The
  root always covers index 0, so the height only grows. Three levels cover
  about 521 million messages.
- Writing a page keeps its ID, so a push changes only the meta page, the
  rightmost index leaf and the tail data page, plus the rightmost page map
  leaf when it starts a data page. A parent changes only when a child is
  added or freed.
- Delivery is at least once. Acknowledgments are cumulative: acknowledging
  a message covers every one before it. A reader that reopens resumes at its
  last acknowledged position.
- Reader positions don't hold back trimming. A reader whose position falls
  below the oldest kept message has missed the messages in between.
- With 4 KiB pages the reader table holds 125 readers, each with a name of
  up to 23 bytes. A push or batch carries up to `max_batch` payload bytes,
  1 MiB by default.

## Tests

```sh
./dev.py test --example --label example
```

The power-loss test runs producers and readers on a fault-injecting file
system, crashes it at a random point and reboots it, dropping or tearing
unsynced writes. After each reboot it checks three things. Every push and
ack that returned success survived. Every kept message is intact and in
order for its producer. No reader received a message that the crash took
back.
