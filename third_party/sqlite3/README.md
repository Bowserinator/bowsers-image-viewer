# sqlite3 (vendored)

The SQLite amalgamation (`sqlite3.c` + `sqlite3.h`), version 3.38.2, public
domain (see `LICENSE`). Used by `src/util/store_sqlite.*` for the app's
persisted settings.

Pulled from https://github.com/azadkuh/sqlite-amalgamation, a mirror of the
official amalgamation published at https://sqlite.org/download.html. Update
by dropping in a newer `sqlite3.c`/`sqlite3.h` pair from either source.

Built as its own small static library in the top-level `CMakeLists.txt`
(target `sqlite3`), with `SQLITE_OMIT_LOAD_EXTENSION` and `SQLITE_DQS=0` set
since the config database never needs extension loading or legacy
double-quoted string literals.
