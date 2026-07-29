// Host test for the review-log and last-known-time persistence in
// persist.h, run against a real temp directory rather than the device.
//
//   c++ -std=c++17 -O2 -Wall -Wextra \
//       -I../include -I../../session/include -I../../deck/include -I../../fsrs/include \
//       test_persist.cpp -o test_persist && ./test_persist

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "persist.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  std::printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
  if (!cond) g_failures++;
}

std::string make_tmp_dir() {
  std::string tmpl = "/tmp/srs_persist_test_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (mkdtemp(buf.data()) == nullptr) {
    std::perror("mkdtemp");
    std::exit(2);
  }
  return std::string(buf.data());
}

long file_size(const char* path) {
  struct stat st;
  if (::stat(path, &st) != 0) return -1;
  return static_cast<long>(st.st_size);
}

void write_raw(const char* path, const void* data, size_t n) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) { std::perror("fopen"); std::exit(2); }
  if (std::fwrite(data, 1, n, f) != n) { std::exit(2); }
  std::fclose(f);
}

persist::Entry make_entry(uint64_t card_id, int64_t reviewed, uint8_t rating,
                          uint8_t state_before, uint16_t duration_ms) {
  persist::Entry e;
  e.card_id = card_id;
  e.reviewed = reviewed;
  e.rating = rating;
  e.state_before = state_before;
  e.duration_ms = duration_ms;
  return e;
}

bool entries_equal(const persist::Entry& a, const persist::Entry& b) {
  return a.card_id == b.card_id && a.reviewed == b.reviewed &&
         a.rating == b.rating && a.state_before == b.state_before &&
         a.duration_ms == b.duration_ms;
}

}  // namespace

int main() {
  const std::string dir = make_tmp_dir();
  std::printf("scratch dir: %s\n", dir.c_str());

  // --- append + replay round trip ------------------------------------------
  {
    const std::string path = dir + "/revlog_roundtrip.bin";
    check(!persist::load(path.c_str()).file_existed,
          "a path with no file yet reports file_existed=false");

    std::vector<persist::Entry> written = {
        make_entry(1, 1780000000, 3, 0, 1200),
        make_entry(2, 1780000060, 1, 0, 800),
        make_entry(1, 1780003600, 3, 1, 500),
        make_entry(3, 1780007200, 4, 0, 2200),
    };
    for (const auto& e : written) {
      check(persist::append(path.c_str(), e), "append succeeds");
    }

    const auto loaded = persist::load(path.c_str());
    check(loaded.file_existed, "file exists after appends");
    check(loaded.header_ok, "header is valid after appends");
    check(!loaded.truncated_tail, "no truncation reported for a clean file");
    check(loaded.entries.size() == written.size(),
          "replay returns exactly what was appended, got " +
              std::to_string(loaded.entries.size()));

    bool all_match = loaded.entries.size() == written.size();
    for (size_t i = 0; all_match && i < written.size(); ++i) {
      if (!entries_equal(written[i], loaded.entries[i])) all_match = false;
    }
    check(all_match, "every field round-trips exactly (card_id, reviewed, rating, state, duration)");
  }

  // --- truncated tail recovery ----------------------------------------------
  {
    const std::string path = dir + "/revlog_truncated.bin";
    persist::append(path.c_str(), make_entry(10, 100, 3, 0, 0));
    persist::append(path.c_str(), make_entry(11, 200, 2, 0, 0));
    const long clean_size = file_size(path.c_str());

    // Simulate a power cut mid-append: a 7-byte fragment of a third record.
    {
      std::FILE* f = std::fopen(path.c_str(), "ab");
      const uint8_t junk[7] = {1, 2, 3, 4, 5, 6, 7};
      std::fwrite(junk, 1, sizeof(junk), f);
      std::fclose(f);
    }
    check(file_size(path.c_str()) == clean_size + 7, "the fragment was actually appended");

    const auto loaded = persist::load(path.c_str());
    check(loaded.header_ok, "a truncated tail does not count as a bad header");
    check(loaded.truncated_tail, "a partial trailing record is detected");
    check(loaded.entries.size() == 2,
          "the two complete records before the fragment survive, got " +
              std::to_string(loaded.entries.size()));

    check(persist::truncate_to_complete_records(path.c_str()),
          "truncation to the last complete record boundary succeeds");
    check(file_size(path.c_str()) == clean_size,
          "the file shrinks back to exactly the two complete records");

    // And a subsequent append lands cleanly, proving the boundary is sane.
    check(persist::append(path.c_str(), make_entry(12, 300, 4, 2, 0)),
          "appending after truncation succeeds");
    const auto reloaded = persist::load(path.c_str());
    check(reloaded.header_ok && !reloaded.truncated_tail && reloaded.entries.size() == 3,
          "after repair, replay sees all three records cleanly");
  }

  // --- corrupt magic rejection ------------------------------------------------
  {
    const std::string path = dir + "/revlog_corrupt.bin";
    const char garbage[20] = "not a revlog at all";
    write_raw(path.c_str(), garbage, sizeof(garbage));

    const auto loaded = persist::load(path.c_str());
    check(loaded.file_existed, "the corrupt file is still seen to exist");
    check(!loaded.header_ok, "a bad magic is rejected rather than misread as history");
    check(loaded.entries.empty(), "no entries are fabricated from a bad header");

    check(!persist::append(path.c_str(), make_entry(99, 1, 1, 0, 0)),
          "append refuses to write onto a file with a bad header");
    check(file_size(path.c_str()) == static_cast<long>(sizeof(garbage)),
          "refusing to append never wipes or shortens the corrupt file");
  }

  // --- reviewed-today across the 4am rollover --------------------------------
  {
    // Mirrors test_session.cpp's day-rollover math: local midnight for day D
    // is D*86400 - offset in unix seconds, and the study day rolls at 4am.
    const int off = 7 * 3600;
    const int64_t D = 20455;
    const int64_t local_midnight = D * 86400 - off;
    const int64_t yesterday_11pm = local_midnight - 1 * 3600;   // still day D-1
    const int64_t three_59am = local_midnight + 4 * 3600 - 60;  // still day D-1
    const int64_t five_am = local_midnight + 5 * 3600;          // day D
    const int64_t eleven_am = local_midnight + 11 * 3600;       // day D

    std::vector<persist::Entry> entries = {
        make_entry(1, yesterday_11pm, 3, 0, 0),
        make_entry(2, three_59am, 3, 0, 0),
        make_entry(3, five_am, 3, 0, 0),
        make_entry(4, eleven_am, 3, 0, 0),
    };

    check(persist::reviewed_today(entries, five_am, off) == 2,
          "querying at 5am counts only the two entries within the post-4am study day");
    check(persist::reviewed_today(entries, three_59am, off) == 2,
          "querying at 3:59am counts the two entries still in the previous study day");
  }

  // --- last-known-time file ----------------------------------------------------
  {
    const std::string path = dir + "/lastknowntime.bin";
    int64_t restored = -1;
    check(!persist::load_time(path.c_str(), &restored),
          "no file yet means no time to restore");

    check(persist::save_time(path.c_str(), 1780012345), "save_time succeeds");
    check(persist::load_time(path.c_str(), &restored) && restored == 1780012345,
          "load_time round-trips the value that was saved");

    check(persist::save_time(path.c_str(), 1780099999), "save_time overwrites cleanly");
    check(persist::load_time(path.c_str(), &restored) && restored == 1780099999,
          "the newer value replaces the old one, not appends to it");

    const char garbage[12] = {0};
    write_raw(path.c_str(), garbage, sizeof(garbage));
    check(!persist::load_time(path.c_str(), &restored),
          "a bad magic in the time file is rejected, not misread as epoch 0");
  }

  std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASS\n", g_failures);
  return g_failures ? 1 : 0;
}
