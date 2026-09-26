#pragma once

// store_sqlite.hpp
//
// SQLiteStore: a Store implementation backed by a single-table SQLite
// database (third_party/sqlite3, the public-domain amalgamation).
//
// Semantics mirror the old text-file store: get/set/erase operate on an
// in-memory cache (m_data) so callers can do many set()s cheaply; load()
// replaces the cache with the full contents of the `kv` table; save()
// writes the cache back to disk inside a single transaction. This keeps
// app.cpp's existing load-once-at-startup / save-once-at-exit usage working
// unchanged against the new backend.
//
// The database is opened once in the constructor and kept open for the
// life of the store (WAL journal mode, so a crash mid-save can't corrupt
// previously-committed data).

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "store.hpp"

// Forward-declared rather than including <sqlite3.h> here so headers that
// only need the Store interface (e.g. app.hpp) don't pull in the C API.
struct sqlite3;

namespace biv {

class SQLiteStore : public Store {
public:
    explicit SQLiteStore(std::filesystem::path path);
    ~SQLiteStore() override;

    SQLiteStore(const SQLiteStore&)            = delete;
    SQLiteStore& operator=(const SQLiteStore&) = delete;
    SQLiteStore(SQLiteStore&&)                 = delete;
    SQLiteStore& operator=(SQLiteStore&&)      = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    // True if the database could not be opened/prepared. Callers can still
    // use the store (get/set operate on the in-memory cache regardless),
    // but load() finds nothing and save() is a no-op, so settings just
    // won't persist across runs.
    [[nodiscard]] bool healthy() const noexcept { return m_db != nullptr; }

    // True if open_db() found the file corrupted and replaced it with a
    // fresh one (see recover_from_corruption()). healthy() is still true in
    // that case -- the new file works fine -- but callers may want to tell
    // the user their settings were reset, and why.
    [[nodiscard]] bool was_recovered() const noexcept override { return m_recovered; }
    [[nodiscard]] std::filesystem::path recovery_backup_path() const override { return m_backup_path; }

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
    void set(std::string_view key, std::string_view value) override;
    bool erase(std::string_view key) override;
    void load() override;
    void save() const override;

private:
    void open_db();
    // Runs PRAGMA quick_check against an already-open handle. Some forms of
    // corruption (bit flips or truncation deep in the data pages) leave the
    // header and schema table readable, so sqlite3_open() and even
    // `CREATE TABLE IF NOT EXISTS` succeed while individual get/save calls
    // fail sporadically afterwards. Catching that up front lets open_db()
    // fail closed instead of leaving the store in a half-working state.
    [[nodiscard]] bool database_is_healthy() const;
    // Moves a corrupted m_path aside and opens/creates a fresh database in
    // its place. On any failure (can't rename, can't create the new file)
    // this leaves m_db null, same as the pre-existing fail-closed path.
    void recover_from_corruption();

    std::filesystem::path m_path;
    sqlite3* m_db = nullptr;
    std::map<std::string, std::string> m_data;
    bool m_recovered = false;
    std::filesystem::path m_backup_path;
};

}  // namespace biv
