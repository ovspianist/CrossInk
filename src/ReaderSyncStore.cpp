#include "ReaderSyncStore.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"

namespace ReaderSyncStore {
namespace {

constexpr char SHARED_ROOT[] = "/.nous-crossink-reader-sync";
constexpr char BOOKS_DIRECTORY[] = "/.nous-crossink-reader-sync/books";
constexpr char COUNTER_PATH[] = "/.nous-crossink-reader-sync/counter-v1.bin";
constexpr char RECORD_SUFFIX[] = ".ncrs";
constexpr std::array<uint8_t, 8> RECORD_MAGIC = {'N', 'C', 'R', 'S', 'Y', 'N', 'C', '1'};
constexpr std::array<uint8_t, 8> COUNTER_MAGIC = {'N', 'C', 'R', 'S', 'E', 'Q', '0', '1'};
constexpr uint8_t RECORD_VERSION = 1;
constexpr size_t HEADER_SIZE = 32;
constexpr size_t MAX_PATH_BYTES = 768;
constexpr size_t MAX_TITLE_BYTES = 512;
constexpr size_t MAX_AUTHOR_BYTES = 512;
constexpr size_t MAX_IMPORTED_RECENTS = 18;

enum class PositionSource : uint8_t {
  None = 0,
  CrossInk = 1,
  Nous = 2,
};

enum class RecentState : uint8_t {
  None = 0,
  Present = 1,
  Removed = 2,
};

struct Record {
  std::string path;
  std::string title;
  std::string author;
  uint32_t recentSequence = 0;
  uint32_t positionSequence = 0;
  uint32_t intraSpinePpm = 0;
  uint16_t spineIndex = 0;
  PositionSource positionSource = PositionSource::None;
  RecentState recentState = RecentState::None;
};

struct RankedRecord {
  uint32_t sequence = 0;
  Record record;
};

uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void writeU16(uint8_t* p, const uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFFu);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void writeU32(uint8_t* p, const uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFFu);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint64_t fnv1a64(const std::string& value) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string canonicalPath(const std::string& path) {
  constexpr char MOUNT[] = "/sdcard";
  constexpr size_t MOUNT_LEN = sizeof(MOUNT) - 1;
  if (path == MOUNT) return "/";
  if (path.size() > MOUNT_LEN && path.compare(0, MOUNT_LEN, MOUNT) == 0 && path[MOUNT_LEN] == '/') {
    return path.substr(MOUNT_LEN);
  }
  return path;
}

bool ensureDirectories() {
  if (!Storage.exists(SHARED_ROOT) && !Storage.mkdir(SHARED_ROOT)) {
    LOG_ERR("RSYNC", "Could not create shared reader-sync directory");
    return false;
  }
  if (!Storage.exists(BOOKS_DIRECTORY) && !Storage.mkdir(BOOKS_DIRECTORY)) {
    LOG_ERR("RSYNC", "Could not create shared reader-sync books directory");
    return false;
  }
  return true;
}

std::string recordPath(const std::string& canonical) {
  char name[32];
  snprintf(name, sizeof(name), "%016llx%s", static_cast<unsigned long long>(fnv1a64(canonical)), RECORD_SUFFIX);
  return std::string(BOOKS_DIRECTORY) + "/" + name;
}

bool readExact(HalFile& file, void* data, const size_t size) {
  return size == 0 || file.read(data, size) == static_cast<int>(size);
}

bool readRecordFile(const std::string& path, Record& record) {
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("RSYNC", path, file)) return false;

  uint8_t header[HEADER_SIZE];
  const bool headerOk = readExact(file, header, sizeof(header));
  if (!headerOk || !std::equal(RECORD_MAGIC.begin(), RECORD_MAGIC.end(), header) || header[8] != RECORD_VERSION) {
    file.close();
    return false;
  }

  const uint16_t pathLen = readU16(header + 26);
  const uint16_t titleLen = readU16(header + 28);
  const uint16_t authorLen = readU16(header + 30);
  const uint8_t rawSource = header[9];
  const uint8_t rawRecentState = header[10];
  const uint32_t recentSequence = readU32(header + 12);
  const size_t expectedSize = HEADER_SIZE + pathLen + titleLen + authorLen;
  if (pathLen == 0 || pathLen > MAX_PATH_BYTES || titleLen > MAX_TITLE_BYTES || authorLen > MAX_AUTHOR_BYTES ||
      readU32(header + 20) > POSITION_SCALE || rawSource > static_cast<uint8_t>(PositionSource::Nous) ||
      rawRecentState > static_cast<uint8_t>(RecentState::Removed) ||
      (recentSequence == 0 && rawRecentState != static_cast<uint8_t>(RecentState::None)) ||
      file.size() != expectedSize) {
    file.close();
    return false;
  }

  record.path.resize(pathLen);
  record.title.resize(titleLen);
  record.author.resize(authorLen);
  const bool bodyOk = readExact(file, record.path.data(), pathLen) && readExact(file, record.title.data(), titleLen) &&
                      readExact(file, record.author.data(), authorLen) && file.available() == 0;
  file.close();
  if (!bodyOk || record.path.empty() || record.path.front() != '/') return false;

  record.recentSequence = recentSequence;
  record.positionSequence = readU32(header + 16);
  record.intraSpinePpm = readU32(header + 20);
  record.spineIndex = readU16(header + 24);
  record.positionSource = static_cast<PositionSource>(rawSource);
  // State 0 with a nonzero sequence is the legacy version-1 representation of
  // an active recent entry.
  record.recentState = rawRecentState == 0 && recentSequence != 0
                           ? RecentState::Present
                           : static_cast<RecentState>(rawRecentState);
  return true;
}

bool readRecord(const std::string& canonical, Record& record) {
  if (!readRecordFile(recordPath(canonical), record)) return false;
  // Refuse to overwrite a different book in the practically impossible event
  // of a 64-bit path-hash collision.
  return record.path == canonical;
}

bool promoteFile(const std::string& tmpPath, const std::string& finalPath) {
  const std::string backupPath = finalPath + ".bak";
  if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());
  const bool hadFinal = Storage.exists(finalPath.c_str()) && Storage.rename(finalPath.c_str(), backupPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    if (hadFinal) Storage.rename(backupPath.c_str(), finalPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (hadFinal) Storage.remove(backupPath.c_str());
  return true;
}

bool writeRecord(const Record& record) {
  if (!ensureDirectories() || record.path.empty() || record.path.size() > MAX_PATH_BYTES ||
      record.title.size() > MAX_TITLE_BYTES || record.author.size() > MAX_AUTHOR_BYTES ||
      record.intraSpinePpm > POSITION_SCALE) {
    return false;
  }

  const std::string finalPath = recordPath(record.path);
  const std::string tmpPath = finalPath + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("RSYNC", tmpPath, file)) return false;

  uint8_t header[HEADER_SIZE] = {};
  std::copy(RECORD_MAGIC.begin(), RECORD_MAGIC.end(), header);
  header[8] = RECORD_VERSION;
  header[9] = static_cast<uint8_t>(record.positionSource);
  header[10] = static_cast<uint8_t>(record.recentState);
  writeU32(header + 12, record.recentSequence);
  writeU32(header + 16, record.positionSequence);
  writeU32(header + 20, record.intraSpinePpm);
  writeU16(header + 24, record.spineIndex);
  writeU16(header + 26, static_cast<uint16_t>(record.path.size()));
  writeU16(header + 28, static_cast<uint16_t>(record.title.size()));
  writeU16(header + 30, static_cast<uint16_t>(record.author.size()));

  const bool wrote = file.write(header, sizeof(header)) == sizeof(header) &&
                     file.write(record.path.data(), record.path.size()) == record.path.size() &&
                     file.write(record.title.data(), record.title.size()) == record.title.size() &&
                     file.write(record.author.data(), record.author.size()) == record.author.size();
  file.flush();
  const bool synced = wrote && file.sync();
  file.close();
  if (!synced) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return promoteFile(tmpPath, finalPath);
}

template <typename Fn>
void forEachRecord(Fn&& fn) {
  FsFile directory = Storage.open(BOOKS_DIRECTORY);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return;
  }

  char name[40];
  constexpr size_t SUFFIX_LEN = sizeof(RECORD_SUFFIX) - 1;
  for (FsFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));
    entry.close();
    if (isDirectory || nameLen <= SUFFIX_LEN || strcmp(name + nameLen - SUFFIX_LEN, RECORD_SUFFIX) != 0) continue;
    Record record;
    const std::string path = std::string(BOOKS_DIRECTORY) + "/" + name;
    if (readRecordFile(path, record) && recordPath(record.path) == path) fn(record);
  }
  directory.close();
}

uint32_t scanMaxSequence() {
  uint32_t maximum = 0;
  forEachRecord([&](const Record& record) {
    maximum = std::max(maximum, record.recentSequence);
    maximum = std::max(maximum, record.positionSequence);
  });
  return maximum;
}

uint32_t readCounter() {
  if (!Storage.exists(COUNTER_PATH)) return 0;
  HalFile file;
  if (!Storage.openFileForRead("RSYNC", COUNTER_PATH, file)) return 0;
  uint8_t data[12];
  const bool ok = readExact(file, data, sizeof(data)) && file.available() == 0 &&
                  std::equal(COUNTER_MAGIC.begin(), COUNTER_MAGIC.end(), data);
  file.close();
  return ok ? readU32(data + 8) : 0;
}

bool writeCounter(const uint32_t value) {
  if (!ensureDirectories()) return false;
  const std::string tmpPath = std::string(COUNTER_PATH) + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("RSYNC", tmpPath, file)) return false;
  uint8_t data[12];
  std::copy(COUNTER_MAGIC.begin(), COUNTER_MAGIC.end(), data);
  writeU32(data + 8, value);
  const bool wrote = file.write(data, sizeof(data)) == sizeof(data);
  file.flush();
  const bool synced = wrote && file.sync();
  file.close();
  if (!synced) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return promoteFile(tmpPath, COUNTER_PATH);
}

uint32_t nextSequence() {
  static bool initialized = false;
  static uint32_t cachedValue = 0;
  if (!initialized) {
    cachedValue = readCounter();
    if (cachedValue == 0) cachedValue = scanMaxSequence();
    initialized = true;
  }
  if (cachedValue == UINT32_MAX) return 0;
  const uint32_t next = cachedValue + 1;
  if (!writeCounter(next)) return 0;
  cachedValue = next;
  return next;
}

void updateMetadata(Record& record, const std::string& canonical, const std::string& title,
                    const std::string& author) {
  record.path = canonical;
  if (!title.empty() && title.size() <= MAX_TITLE_BYTES) record.title = title;
  if (!author.empty() && author.size() <= MAX_AUTHOR_BYTES) record.author = author;
}

const RecentBook* findRecentByPath(const std::vector<RecentBook>& books, const std::string& path) {
  const auto it = std::find_if(books.begin(), books.end(), [&](const RecentBook& book) { return book.path == path; });
  return it == books.end() ? nullptr : &*it;
}

void insertRanked(std::vector<RankedRecord>& ranked, Record record) {
  RankedRecord candidate{record.recentSequence, std::move(record)};
  if (ranked.size() == MAX_IMPORTED_RECENTS) {
    if (candidate.sequence <= ranked.back().sequence) return;
    ranked.pop_back();
  }
  auto insertion = std::lower_bound(ranked.begin(), ranked.end(), candidate,
                                    [](const RankedRecord& lhs, const RankedRecord& rhs) {
                                      return lhs.sequence > rhs.sequence;
                                    });
  ranked.insert(insertion, std::move(candidate));
}

}  // namespace

void synchronizeRecentBooks() {
  if (!ensureDirectories()) return;

  const auto nativeBooks = RECENT_BOOKS.getBooks();
  // Bootstrap from oldest to newest so the existing CrossInk order survives
  // the one-time migration into the shared sequence space.
  for (auto it = nativeBooks.rbegin(); it != nativeBooks.rend(); ++it) {
    const std::string canonical = canonicalPath(it->path);
    if (canonical.empty() || canonical.front() != '/' || canonical.size() > MAX_PATH_BYTES) continue;
    Record record;
    if (!readRecord(canonical, record)) record.path = canonical;
    bool changed = false;
    if (record.recentSequence == 0) {
      record.recentSequence = nextSequence();
      record.recentState = RecentState::Present;
      changed = record.recentSequence != 0;
    }
    updateMetadata(record, canonical, it->title, it->author);

    if (record.positionSequence == 0) {
      Epub epub(canonical, "/.crosspoint");
      EpubReaderUtils::Progress progress;
      if (EpubReaderUtils::loadProgress(epub, progress) && progress.hasPageCount && progress.pageCount > 0 &&
          progress.spineIndex >= 0 && progress.spineIndex <= UINT16_MAX) {
        const uint32_t positionSequence = nextSequence();
        if (positionSequence != 0) {
          record.positionSequence = positionSequence;
          record.positionSource = PositionSource::CrossInk;
          record.spineIndex = static_cast<uint16_t>(progress.spineIndex);
          record.intraSpinePpm =
              progress.pageCount > 1
                  ? static_cast<uint32_t>(static_cast<uint64_t>(std::max(0, progress.pageNumber)) * POSITION_SCALE /
                                          static_cast<uint32_t>(progress.pageCount - 1))
                  : 0;
          record.intraSpinePpm = std::min(record.intraSpinePpm, POSITION_SCALE);
          changed = true;
        }
      }
    }
    if (changed && !writeRecord(record)) LOG_ERR("RSYNC", "Could not bootstrap %s", canonical.c_str());
  }

  std::vector<RankedRecord> ranked;
  ranked.reserve(MAX_IMPORTED_RECENTS);
  bool hasRecentEvent = false;
  forEachRecord([&](const Record& record) {
    if (record.recentSequence == 0) return;
    hasRecentEvent = true;
    if (record.recentState != RecentState::Present || record.path.empty() || !Storage.exists(record.path.c_str())) {
      return;
    }
    insertRanked(ranked, record);
  });
  if (!hasRecentEvent) return;

  std::vector<RecentBook> imported;
  imported.reserve(ranked.size());
  for (const auto& item : ranked) {
    std::string coverPath;
    if (const RecentBook* existing = findRecentByPath(nativeBooks, item.record.path)) {
      coverPath = existing->coverBmpPath;
    }
    imported.push_back({item.record.path, item.record.title, item.record.author, std::move(coverPath)});
  }
  RECENT_BOOKS.replaceFromSync(std::move(imported));
}

bool recordBookOpened(const std::string& path, const std::string& title, const std::string& author) {
  if (!ensureDirectories()) return false;
  const std::string canonical = canonicalPath(path);
  if (canonical.empty() || canonical.front() != '/' || canonical.size() > MAX_PATH_BYTES) return false;
  Record record;
  if (!readRecord(canonical, record)) record.path = canonical;
  const uint32_t sequence = nextSequence();
  if (sequence == 0) return false;
  updateMetadata(record, canonical, title, author);
  record.recentSequence = sequence;
  record.recentState = RecentState::Present;
  return writeRecord(record);
}

bool recordBookRemoved(const std::string& path) {
  if (!ensureDirectories()) return false;
  const std::string canonical = canonicalPath(path);
  if (canonical.empty() || canonical.front() != '/' || canonical.size() > MAX_PATH_BYTES) return false;
  Record record;
  if (!readRecord(canonical, record)) record.path = canonical;
  if (record.recentSequence != 0 && record.recentState == RecentState::Removed) return true;
  const uint32_t sequence = nextSequence();
  if (sequence == 0) return false;
  record.recentSequence = sequence;
  record.recentState = RecentState::Removed;
  return writeRecord(record);
}

bool loadNousPosition(const std::string& path, Position& position) {
  const std::string canonical = canonicalPath(path);
  Record record;
  if (!readRecord(canonical, record) || record.positionSequence == 0 ||
      record.positionSource != PositionSource::Nous) {
    return false;
  }
  position.spineIndex = record.spineIndex;
  position.intraSpinePpm = record.intraSpinePpm;
  return true;
}

bool saveCrossInkPosition(const std::string& path, const std::string& title, const std::string& author,
                          const int spineIndex, const int pageNumber, const int pageCount) {
  if (spineIndex < 0 || spineIndex > UINT16_MAX || pageNumber < 0 || pageCount <= 0) return false;
  const std::string canonical = canonicalPath(path);
  if (canonical.empty() || canonical.front() != '/' || canonical.size() > MAX_PATH_BYTES) return false;

  const uint32_t intra =
      pageCount > 1
          ? static_cast<uint32_t>(static_cast<uint64_t>(pageNumber) * POSITION_SCALE /
                                  static_cast<uint32_t>(pageCount - 1))
          : 0;
  Record record;
  if (!readRecord(canonical, record)) record.path = canonical;
  updateMetadata(record, canonical, title, author);
  const uint32_t clampedIntra = std::min(intra, POSITION_SCALE);
  if (record.positionSource == PositionSource::CrossInk && record.spineIndex == spineIndex &&
      record.intraSpinePpm == clampedIntra) {
    return true;
  }
  const uint32_t sequence = nextSequence();
  if (sequence == 0) return false;
  record.positionSequence = sequence;
  record.positionSource = PositionSource::CrossInk;
  record.spineIndex = static_cast<uint16_t>(spineIndex);
  record.intraSpinePpm = clampedIntra;
  return writeRecord(record);
}

}  // namespace ReaderSyncStore
