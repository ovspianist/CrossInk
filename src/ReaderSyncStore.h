#pragma once

#include <cstdint>
#include <string>

namespace ReaderSyncStore {

constexpr uint32_t POSITION_SCALE = 1000000;

struct Position {
  uint16_t spineIndex = 0;
  uint32_t intraSpinePpm = 0;
};

// Bootstrap CrossInk's native recents into the shared store, then replace the
// native list with the newest 18 shared EPUB records.
void synchronizeRecentBooks();

// Add a shared recency event for a book CrossInk has just opened.
bool recordBookOpened(const std::string& path, const std::string& title, const std::string& author);

// Add a shared tombstone after the user removes a book from Recent Books.
// The book and its shared reading position remain intact.
bool recordBookRemoved(const std::string& path);

// Load a position only when Nous wrote the latest shared position.
bool loadNousPosition(const std::string& path, Position& position);

// Export a rendered CrossInk page as spine-relative parts-per-million.
bool saveCrossInkPosition(const std::string& path, const std::string& title, const std::string& author,
                          int spineIndex, int pageNumber, int pageCount);

}  // namespace ReaderSyncStore
