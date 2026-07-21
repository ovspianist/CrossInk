# Nous–CrossInk Reader Sync Roadmap

Last updated: 2026-07-21
Status: Recent-removal sync implemented; on-device validation pending
Scope: CrossInk and Nous firmware interoperability on one Xteink X4 SD card

## Why this exists

- Switching OTA app partitions currently separates recent-book history between the two firmwares.
- CrossInk stores EPUB positions as rendered pages, while Nous stores chapter paragraph/text offsets.
- The user wants approximate resume positions and a shared recent-books experience without coupling either firmware to the other's private data files.

## Product goals

- Share recent EPUB openings in both directions.
- Resume in approximately the same location after switching firmware.
- Keep either firmware fully usable when the other firmware or shared data is absent.
- Show complementary chapter and book progress bars in Nous when bar progress is enabled.

## Non-goals

- Pixel-, line-, or page-exact matching between two different renderers.
- Synchronizing reading statistics, bookmarks, annotations, settings, or non-EPUB documents.
- Replacing either firmware's native progress or recent-books storage.

## Constraints and guardrails

- Performance: sync directory scans run only at library/reader lifecycle boundaries, never while rendering a page.
- Correctness: malformed or partial shared records are ignored; native data remains authoritative and intact.
- Compatibility: the shared root is exactly `/.nous-crossink-reader-sync/`; Nous accesses it through its `/sdcard` mount.
- Memory: CrossInk retains no more than its native 18 recent entries and parses one small record at a time.
- Persistence: small per-book records use temporary-file promotion; no progress write occurs on every page paint.

## Key architecture decisions

- `NCXRS-ADR-01` Use one versioned binary record per canonical SD-relative EPUB path.
  - Why: bounded reads, no escaping problems, and no need for either firmware to parse the other's private formats.
  - Alternative rejected: a monolithic JSON snapshot would force CrossInk to allocate metadata for Nous's full 250-book index.

- `NCXRS-ADR-02` Normalize Nous `/sdcard/...` paths to CrossInk-style `/...` paths and name records with a shared FNV-1a 64-bit path hash.
  - Why: both firmwares refer to the same physical SD files through different mount prefixes.
  - Alternative rejected: title/author identity can collide and changes when metadata parsing differs.

- `NCXRS-ADR-03` Exchange `spine index + intra-spine parts-per-million` as the baseline position.
  - Why: both parsers preserve OPF spine order, while their page and paragraph coordinates are incompatible.
  - Alternative rejected: directly copying page or paragraph numbers produces incorrect resumes.

- `NCXRS-ADR-04` Track the position origin and ignore a firmware's own last export.
  - Why: prevents repeatedly re-importing and rounding its own approximate coordinate.
  - Alternative rejected: FAT timestamps are not a reliable conflict clock on the X4.

- `NCXRS-ADR-05` Represent recent-list removal as an explicit, sequenced tombstone in the existing reserved record byte.
  - Why: a missing native recent entry is ambiguous because it may have been evicted by a size limit, never imported, or deliberately removed.
  - Compatibility: existing version-1 records with a zero state byte remain active when their recent sequence is nonzero; older firmware ignores the new state byte.
  - Alternative rejected: deleting the shared record would also discard position data and could be mistaken for an uninitialized book.

## Open design questions

- [x] `NCXRS-DQ-01` Shared directory name is `/.nous-crossink-reader-sync/`.
- [x] `NCXRS-DQ-02` Approximate spine-percentage resume accuracy is acceptable.

## Phase 0: Baseline / diagnostics

- [x] `NCXRS-BASE-01` Document both native recent-book and EPUB position formats.
- [x] `NCXRS-BASE-02` Confirm both parsers preserve OPF spine order.

## Phase 1: Shared protocol foundation

- [x] `NCXRS-DATA-01` Add compatible record readers/writers and path normalization in Nous.
- [x] `NCXRS-DATA-02` Add compatible record readers/writers and path normalization in CrossInk.
- [x] `NCXRS-DATA-03` Reject corrupt, oversized, unsupported, incomplete, or misnamed records safely.

## Phase 2: Recent books

- [x] `NCXRS-RECENT-01` Export Nous openings and bootstrap existing Nous recent entries.
- [x] `NCXRS-RECENT-02` Import shared recency order into Nous without resetting reading-time statistics.
- [x] `NCXRS-RECENT-03` Export CrossInk openings and bootstrap its existing 18-entry recent list.
- [x] `NCXRS-RECENT-04` Import the newest 18 shared entries into CrossInk in one persistent update.

## Phase 3: Reading positions

- [x] `NCXRS-POS-01` Export Nous MRB character progress as spine-relative parts-per-million.
- [x] `NCXRS-POS-02` Import a CrossInk position into Nous paragraph/text coordinates on book open.
- [x] `NCXRS-POS-03` Export CrossInk rendered-page progress as spine-relative parts-per-million.
- [x] `NCXRS-POS-04` Import a Nous position into CrossInk after the target section page count is known.

## Phase 4: Nous complementary progress bar

- [x] `NCXRS-UX-01` Preserve the configured bottom progress bar.
- [x] `NCXRS-UX-02` Draw the opposite scope as a thin top-edge bar when bar progress is enabled.
- [x] `NCXRS-UX-03` Verify portrait, flipped, and landscape layout bounds in the shared rotation-aware draw buffer.

## Phase 5: Recent-list removals

- [x] `NCXRS-RECENT-05` Export an explicit tombstone when the user removes an EPUB from CrossInk's list or grid Recent Books view.
- [x] `NCXRS-RECENT-06` Exclude tombstoned records when CrossInk rebuilds its native recent list.
- [x] `NCXRS-RECENT-07` Clear Nous's native recent order when a newer shared record is tombstoned, without deleting the library entry or reading statistics.
- [x] `NCXRS-RECENT-08` Reopening a tombstoned book in either firmware restores it as an active recent entry.

## Acceptance criteria

- [ ] `NCXRS-AC-01` Opening an EPUB in either firmware places it in both recent-book views after switching.
- [ ] `NCXRS-AC-02` A position exported by either firmware resumes in the same spine and approximately the same location in the other.
- [ ] `NCXRS-AC-03` Corrupt or missing sync data never prevents booting, browsing, or opening a book.
- [ ] `NCXRS-AC-04` Nous displays both chapter and book progress bars only when `ProgressStyle::Bar` is selected.
- [x] `NCXRS-AC-05` Both X4 firmware builds complete and produce flashable binaries.
- [ ] `NCXRS-AC-06` Removing a recent EPUB in CrossInk removes it from both recent views after switching to Nous, while the book remains in the library.

## Testing checklist

- [x] `NCXRS-QA-01` Host protocol test covers binary layout, foreign-position import, path normalization, ordering, statistics preservation, and malformed/misnamed records.
- [ ] `NCXRS-QA-02` Nous unit/desktop checks cover position translation and dual progress rendering logic.
- [ ] `NCXRS-QA-03` CrossInk simulator/default builds cover integration and embedded APIs.
- [x] `NCXRS-QA-04` Both PlatformIO firmware builds pass and artifacts are identified.
- [x] `NCXRS-QA-05` Host protocol test covers active, removed, and reopened recent states plus invalid-state rejection.

## Session log

- [x] `NCXRS-LOG-20260721-01` Baseline and protocol design
  - Changes:
  - Confirmed native formats, identity differences, OPF spine compatibility, and renderer limitations.
  - Chose bounded per-book binary records under `/.nous-crossink-reader-sync/`.
  - Verification:
  - Repository source inspection (pass).
  - No build commands run yet.
  - Next task IDs:
  - `NCXRS-DATA-01`, `NCXRS-RECENT-01`, `NCXRS-POS-01`.

- [x] `NCXRS-LOG-20260721-02` Bidirectional implementation and build handoff
  - Changes:
  - Added compatible bounded reader-sync stores, initial migration, shared recency ordering, and approximate resume translation to both firmwares.
  - Added Nous's complementary top progress bar for Bar mode.
  - Documented the shared binary contract and packaged both flashable images.
  - Verification:
  - Host interoperability test (pass).
  - CrossInk `pio run -e tiny` (pass; 5,501,792-byte image, 1,051,808 bytes free in the OTA app partition).
  - Nous `pio run` (pass; 3,808,418 bytes reported flash use, 127,168 bytes RAM use).
  - CrossInk `pio run -e simulator` compiled the new sync and reader objects, then stopped on the existing simulator `WiFi.disconnect` stub/API mismatch.
  - Next task IDs:
  - `NCXRS-AC-01` through `NCXRS-AC-04` require two-firmware testing on the X4.

- [x] `NCXRS-LOG-20260721-03` Reliable recent-removal tombstones
  - Changes:
  - Used the existing version-1 header's reserved byte for a sequenced present/removed state, retaining the 32-byte record and backward readability.
  - CrossInk list and grid removal actions now write tombstones; both firmware importers suppress tombstoned recents while retaining the shared position and native library/statistics data.
  - Reopening a book writes a new present event and restores it to recents.
  - Verification:
  - Host tombstone lifecycle and invalid-state tests (pass).
  - CrossInk `pio run -e tiny` (pass; 5,502,128-byte image, 1,051,472 bytes free; no reported RAM increase and 336 bytes more image data than the prior sync build).
  - Nous `pio run` (pass; 3,808,544 bytes reported flash use, 127,168 bytes RAM use; no reported RAM increase).
  - Next task IDs:
  - `NCXRS-AC-06` requires removal and reopen testing across both app partitions on the X4.
