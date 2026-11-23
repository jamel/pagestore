# PageStore

PageStore is a C11 library for managing fixed-size pages in an embedded storage
engine. It handles page allocation and caching, groups updates into operations
that callers can commit or abort, and checkpoints committed changes to disk.
The public API is in [`pagestore.h`](include/pagestore/pagestore.h).

See examples implemented on top of PageStore:
 - [`examples/btree`](examples/btree) builds a B+tree and a key-value tool on it.
 - [`examples/queue`](examples/queue) builds a durable message queue for threads.

## Development

You'll need Python 3.8+, CMake 3.20+, Ninja, and GCC or Clang. The `tsan`,
`msan` and `coverage` presets use Clang, and `coverage` also needs `llvm-cov`
and `llvm-profdata`. Run `msan` on Linux.

```sh
./dev.py build                       # Configure and build debug
./dev.py build --example             # Also build the examples in examples/
./dev.py test                        # Build and run the debug tests
./dev.py test --list                 # List test names without building or running them
./dev.py test checkpoint --no-build  # Rerun matching tests from the last build
./dev.py test checkpoint --verbose   # Rerun matching tests with their output
./dev.py test -p asan                # Build and test with ASan and UBSan
./dev.py test --example              # Also build and test the examples
./dev.py test --coverage             # Run the tests and write a coverage report
./dev.py test wal --coverage         # See which lines the matching tests cover
./dev.py test -p all                 # Build and test every preset
./dev.py clean -p all                # Clean every build preset
```

Use `--release` for the release preset. Run `./dev.py presets` to see the full
list of available presets.

`--coverage` prints a summary for each library source file and writes an HTML
report with line counts to `build/coverage/coverage/html/index.html`. The report
covers only the tests in that run. Files those tests never reach show 0%, so
gaps are easy to spot.
