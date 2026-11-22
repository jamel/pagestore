# PageStore

PageStore is a C11 library for managing fixed-size pages in an embedded storage
engine. It handles page allocation and caching, groups updates into operations
that callers can commit or abort, and checkpoints committed changes to disk.
The public API is in [`pagestore.h`](include/pagestore/pagestore.h).

## Development

You'll need Python 3.8+, CMake 3.20+, Ninja, and GCC or Clang. The `tsan` and
`msan` presets use Clang. Run `msan` on Linux.

```sh
./dev.py build                       # Configure and build debug
./dev.py test                        # Build and run the debug tests
./dev.py test --list                 # List test names without building or running them
./dev.py test checkpoint --no-build  # Rerun matching tests from the last build
./dev.py test checkpoint --verbose   # Rerun matching tests with their output
./dev.py test -p asan                # Build and test with ASan and UBSan
./dev.py test -p all                 # Build and test every preset
./dev.py clean -p all                # Clean every build preset
```

Use `--release` for the release preset. Run `./dev.py presets` to see the full
list of available presets.
