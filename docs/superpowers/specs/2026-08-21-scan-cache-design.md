# Scan cache (upstream-1999 TODO "scan caching") — design

**Status:** approved for implementation (autonomous session 2026-08-21; user
asked for this TODO item to be executed and is reviewing asynchronously —
decisions below are self-approved with rationale and can be amended on
review).

**Ticket:** TODO.md, "Still-relevant items from the 1999 upstream TODO":

> **Scan caching** (`~/.fsvcache/@usr@lib`-style): store the tree state,
> re-scan only subdirectories with updated timestamps on the next launch;
> would also let ColorByTimestamp use the previous scan as the spectrum
> start ("graphical diff" of the filesystem).

## What the scan actually costs

`scanfs()` (src/scanfs.c) does one `scandir()` per directory plus one
`lstat()` per entry, depth-first, building the `GNode` tree of
`NodeDesc`/`DirNodeDesc`. Post-scan, `setup_fstree_recursive()` sorts every
directory and derives all subtree aggregates — those are cheap CPU passes
over in-memory data and stay untouched. The cache targets the syscalls.

## Semantics (the heart of the design)

A directory's mtime changes when entries are added, removed or renamed in
it — and does NOT change when a contained file is edited in place, nor
when anything happens deeper in the subtree. Two consequences the design
embraces rather than fights:

1. **Every directory is still `lstat()`ed on every scan.** An unchanged
   ancestor says nothing about its descendants, so subtree-skipping by the
   root's mtime alone would be wrong. What the cache legitimately skips is
   `scandir()` on unchanged directories and `lstat()` on the FILES inside
   them — files are the overwhelming majority of nodes, so most of the
   syscall bill still disappears.
2. **In-place file edits inside an unchanged directory are invisible to
   the cache** (stale size/mtime until the parent directory's own mtime
   moves or the user forces a fresh scan). This is the trade-off the 1999
   ticket itself describes. It is disclosed in TODO.md, locked in by a
   test that asserts the staleness (so a future change to the semantics is
   a deliberate act), and bounded by Rescan: the explicit File → Rescan
   action always bypasses the cache read side.

Race guard: a directory whose cached mtime is within 1 second of the
cache's own scan timestamp is treated as changed (same-second-modification
race, the classic git index problem). Costs a handful of false re-scans,
never a false hit.

## Cache file

- **Location:** `$FSV_CACHE_DIR` if set (tests), else
  `g_get_user_cache_dir()/fsv/` (macOS: `~/Library/Caches/fsv/`; XDG
  elsewhere). One file per scanned root, named by the upstream ticket's
  own convention: absolute path with `/` → `@` (`@Users@willow@src`).
  Deviation from the ticket's literal `~/.fsvcache`: GLib's cache dir is
  the platform-idiomatic place; the `@` naming is kept.
- **Format:** versioned native-endian binary; a cache is a per-machine
  artifact, safe to discard on any mismatch. Header: magic `FSVC`,
  version u32, endian marker u32 (0x01020304), scan wall time i64, root
  path (u32 len + bytes), exclusion fingerprint (u32 len + bytes — the
  active exclusion state: on/off, `FSV_NO_EXCLUDE`, and every user
  `--exclude` pattern; built-ins are compile-time but fingerprinted anyway
  so upgrading fsv invalidates cleanly via the version bump).
  Then the root directory record, depth-first:
  `u8 type, i64 size, i64 size_alloc, u32 uid, u32 gid, i64 atime,
  i64 mtime, i64 ctime, u16 name_len, name bytes`, and for directories a
  trailing `u32 n_children` followed by the children records.
- **Write:** after `setup_fstree_recursive()` completes (tree is final),
  to a temp file in the same directory, `rename()` into place. Best
  effort: any failure logs and moves on — a cache must never break a scan.
- **Read:** whole file into memory, bounds-checked parse into an index of
  `FscacheDir`/`FscacheEntry` structs (per-directory name→child hash for
  O(1) lookup during replay). Any parse anomaly, version/endian/root/
  fingerprint mismatch → discard entirely, full scan. The index is
  transient: released as soon as the scan finishes (memory ~= one extra
  copy of the tree's scalars during the scan; acceptable, disclosed).

## Module: src/fscache.c + fscache.h (libfsvcore)

~API:

    void      fscache_set_enabled(boolean);     /* --no-cache / FSV_NO_CACHE */
    boolean   fscache_get_enabled(void);
    void      fscache_skip_next_load(void);     /* Rescan: read-side bypass, one shot */
    boolean   fscache_load(const char *abs_root);
    time_t    fscache_prev_scan_time(void);     /* 0 when no cache loaded this scan */
    const FscacheDir *fscache_root(void);
    const FscacheDir *fscache_dir_child(const FscacheDir *, const char *name);
    boolean   fscache_dir_matches(const FscacheDir *, time_t mtime, time_t ctime);
    /* entry iteration: count + indexed accessor returning the scalars */
    void      fscache_save(GNode *root_dnode, const char *abs_root);
    void      fscache_release(void);
    int       fscache_replayed_dirs(void);      /* test/observability counter */

`FSV_NO_CACHE` (env, any value) mirrors `FSV_NO_EXCLUDE`'s role: the
GTK arm gets an off switch without new UI; the SDL arm additionally gets a
`--no-cache` CLI flag (SDL-only, like `--exclude` — disclosed).

## scanfs() integration

`process_dir()` gains a `const FscacheDir *cdir` parameter (NULL = no
cache coverage for this directory):

- **Replay path** — `cdir` non-NULL and `fscache_dir_matches(cdir,
  st_mtime, st_ctime)` for the directory's fresh lstat (taken by the
  parent loop as today): iterate cached entries instead of `scandir()`.
  Files get their NodeDesc scalars straight from the cache (no lstat).
  Subdirectories are lstat()ed fresh (their own match test needs current
  mtime; also keeps their displayed metadata true) and recursed with their
  cached counterpart. A cached subdirectory whose lstat now fails is
  skipped (mirrors the existing stat-failure arm).
- **Scan path** — `scandir()` exactly as today; when recursing into a
  subdirectory, pass `fscache_dir_child(cdir, name)` so unchanged
  subtrees under a changed directory still replay.
- Both paths keep the existing per-node bookkeeping identical:
  `dirtree_entry_new()`, node counts, `gui_update()`, id assignment.

In `scanfs()`: after `root_dir` is resolved, `fscache_load(root_dir)`;
root scan runs with `fscache_root()`; after `setup_fstree_recursive()`,
`fscache_save(root_dnode, root_dir)` then `fscache_release()`.

## Graphical diff (the ticket's second half)

`fscache_prev_scan_time()` exposes the loaded cache's header timestamp for
the duration of the run. The SDL Color Setup dialog's "By date/time" tab
gains a "Since previous scan" preset that sets `old_time` to it and
`new_time` to now — files touched since the last fsv run light up as the
spectrum's new end. Hidden (or disabled) when no previous scan time
exists. No core color.c change: `old_time`/`new_time` already exist.

## Rescan / Change Root

- File → Rescan: calls `fscache_skip_next_load()` before requesting the
  scan — the user asked for disk truth. The scan still SAVES afterward.
- Change Root and initial load: cache read enabled (navigation, not
  refresh).

## Testing (meson test `fscache`, same headless recipe as `scanfs_exclude`)

Runtime-built tmp tree, `FSV_CACHE_DIR` pointed into the tmp dir. After
building the fixture, all directory mtimes are backdated (`utimes`, −10s)
so subsequent modifications reliably move them (second-granularity mtimes
plus the race guard would otherwise make same-second tests flaky).

1. **Roundtrip:** scan → cache file exists; scan again → tree identical
   (recursive name/type/size compare), `fscache_replayed_dirs() > 0`.
2. **Add/remove propagates:** add a file in a subdir (bumps its mtime) →
   next scan shows it; the sibling dirs still replay.
3. **Staleness contract:** grow a file in place (parent dir mtime
   backdated again) → next scan still shows the OLD size, and a
   subsequent skip-load scan (Rescan semantics) shows the new one.
4. **Fingerprint mismatch:** add an `--exclude` pattern → full scan
   (`fscache_replayed_dirs() == 0`), cache rewritten with new fingerprint.
5. **Disabled:** `fscache_set_enabled(FALSE)` → no file written, no replay.
6. **Corrupt cache:** truncate the file → full scan, no crash.

## Phases (≤5 files each, commit per phase)

- **A** — fscache module + save/load/parse unit surface + meson + test
  skeleton (tests 1, 5, 6 against fscache directly where possible).
- **B** — scanfs integration (replay/scan paths) + tests 2, 3, 4.
- **C** — SDL wiring: `--no-cache`, Rescan skip, "Since previous scan"
  color preset; docs (TODO.md, PORTING.md).

## Out of scope

- Background re-verify after a cached load (correct-but-slow second pass).
- Watching (FSEvents/inotify).
- Cross-machine/portable cache format.
- GTK-side CLI flag or dialog work (env vars only; disclosed, matching the
  `--exclude` precedent).
