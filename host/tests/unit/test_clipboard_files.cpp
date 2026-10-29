/**
 * @file tests/unit/test_clipboard_files.cpp
 * @brief Test src/clipboard_files.*: what a client may read from and write to this host's disk.
 */
#include "../tests_common.h"

#include <src/clipboard_files.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace clipboard_files;
namespace fs = std::filesystem;

namespace {
  struct scratch_t {
    fs::path root;

    scratch_t() {
      std::random_device rd;
      root = fs::temp_directory_path() / ("clipboard-files-test-" + std::to_string(rd()) + std::to_string(rd()));
      fs::create_directories(root);
    }

    ~scratch_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };

  void put(const fs::path &p, const std::string &content) {
    std::ofstream(p, std::ios::binary) << content;
  }

  std::string get(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }
}  // namespace

TEST(ClipboardFilesNames, AcceptsOrdinaryNames) {
  for (const char *name : {"file.txt", ".hidden", "console.txt", "comX", "com10", "a b", "\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 \xE6\x96\x87\xE4\xBB\xB6.txt", "emoji \xF0\x9F\x98\x80"}) {
    EXPECT_TRUE(valid_name(name)) << name;
  }
}

TEST(ClipboardFilesNames, RefusesNamesThatEscapeOrAlias) {
  const std::string too_long(max_name_bytes + 1, 'a');
  for (const std::string &name : {std::string(), std::string("."), std::string(".."), std::string("a/b"), std::string("a\\b"), std::string("a:b"), std::string("ads:stream"), std::string("a*"), std::string("a?"), std::string("a\"b"), std::string("a<b"), std::string("a|b"), std::string("tab\there"), std::string("nl\n"), std::string("del\x7f"), std::string("trailing."), std::string("trailing "), too_long}) {
    EXPECT_FALSE(valid_name(name)) << name;
  }
}

TEST(ClipboardFilesNames, RefusesWindowsDeviceNames) {
  for (const char *name : {"con", "CON", "Con.txt", "nul", "nul .log", "prn.x.y", "aux", "com1", "COM9.dat", "lpt0", "conin$", "CONOUT$", "com\xC2\xB9", "lpt\xC2\xB3.txt"}) {
    EXPECT_FALSE(valid_name(name)) << name;
  }
}

TEST(ClipboardFilesNames, RefusesMalformedUtf8) {
  for (const char *name : {"\xC0\xAE", "\xE0\x80\xAE", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF", "abc\xC3", "\x80"}) {
    EXPECT_FALSE(valid_name(name)) << "bytes " << name;
  }
}

TEST(ClipboardFilesPaths, Validation) {
  EXPECT_TRUE(valid_relative_path("a/b/c.txt"));
  for (const char *path : {"", "/a", "a/", "a//b", "../a", "a/../b", "a/./b", "a\\..\\b", "C:/x", "a/con/b"}) {
    EXPECT_FALSE(valid_relative_path(path)) << path;
  }
  std::string deep = "a";
  for (std::size_t i = 1; i < max_depth; ++i) {
    deep += "/a";
  }
  EXPECT_TRUE(valid_relative_path(deep));
  EXPECT_FALSE(valid_relative_path(deep + "/a"));
  EXPECT_FALSE(valid_relative_path(std::string(max_path_bytes + 1, 'a')));
}

TEST(ClipboardFilesEntries, Validation) {
  EXPECT_TRUE(valid_entries({{"dir", 0, true}, {"dir/a.txt", 3, false}, {"top.txt", 1, false}}));
  EXPECT_FALSE(valid_entries({})) << "nothing";
  EXPECT_FALSE(valid_entries({{"dir/a.txt", 3, false}})) << "parent never listed";
  EXPECT_FALSE(valid_entries({{"dir/a.txt", 3, false}, {"dir", 0, true}})) << "parent listed after";
  EXPECT_FALSE(valid_entries({{"f", 1, false}, {"f/x", 1, false}})) << "a file as a parent";
  EXPECT_FALSE(valid_entries({{"A.txt", 1, false}, {"a.TXT", 1, false}})) << "same name to Windows";
  EXPECT_FALSE(valid_entries({{"dir", 5, true}})) << "folder with a size";
  EXPECT_FALSE(valid_entries({{"a", max_total_bytes, false}, {"b", 1, false}})) << "too large in total";
  EXPECT_FALSE(valid_entries({{"a", ~0ull, false}, {"b", 2, false}})) << "size overflow";
  std::vector<entry_t> many;
  for (std::size_t i = 0; i <= max_entries; ++i) {
    many.push_back({"f" + std::to_string(i), 0, false});
  }
  EXPECT_FALSE(valid_entries(many)) << "too many";
}

TEST(ClipboardFilesUpload, WritesExactlyWhatWasDeclared) {
  scratch_t s;
  uploads_t uploads;
  const std::vector<entry_t> entries {{"dir", 0, true}, {"dir/a.txt", 5, false}, {"empty", 0, false}, {"b.bin", 4, false}};
  const auto begun = uploads.begin("client-1", entries, s.root, ~0ull);
  ASSERT_FALSE(begun.id.empty()) << begun.error;
  EXPECT_EQ(begun.id.size(), 32u);

  std::string why;
  EXPECT_FALSE(uploads.write("client-2", begun.id, 1, 0, "hello", &why)) << "another client";
  EXPECT_FALSE(uploads.write("client-1", begun.id, 0, 0, "x", &why)) << "a folder";
  EXPECT_FALSE(uploads.write("client-1", begun.id, 9, 0, "x", &why)) << "no such index";
  EXPECT_TRUE(uploads.write("client-1", begun.id, 1, 0, "hel", &why)) << why;
  EXPECT_FALSE(uploads.write("client-1", begun.id, 1, 0, "hel", &why)) << "repeat of the first chunk";
  EXPECT_FALSE(uploads.write("client-1", begun.id, 1, 2, "llo", &why)) << "overlapping chunk";
  EXPECT_FALSE(uploads.write("client-1", begun.id, 1, 3, "lo!", &why)) << "past the declared size";
  EXPECT_TRUE(uploads.write("client-1", begun.id, 1, 3, "lo", &why)) << why;

  std::vector<fs::path> tops, files;
  EXPECT_FALSE(uploads.commit("client-1", begun.id, tops, files, &why)) << "b.bin incomplete";
  EXPECT_TRUE(uploads.write("client-1", begun.id, 3, 0, std::string_view("\x00\x01\x02\x03", 4), &why)) << why;
  EXPECT_FALSE(uploads.commit("client-2", begun.id, tops, files, &why)) << "another client";
  ASSERT_TRUE(uploads.commit("client-1", begun.id, tops, files, &why)) << why;

  const auto folder = s.root / begun.id;
  EXPECT_EQ(get(folder / "dir" / "a.txt"), "hello");
  EXPECT_EQ(get(folder / "b.bin"), std::string("\x00\x01\x02\x03", 4));
  EXPECT_TRUE(fs::exists(folder / "empty"));
  EXPECT_EQ(tops, (std::vector<fs::path> {folder / "dir", folder / "empty", folder / "b.bin"}));
  EXPECT_EQ(files.size(), 3u);
  EXPECT_EQ(uploads.active(), 0u);
#ifndef _WIN32
  struct stat st {};
  ASSERT_EQ(stat((folder / "b.bin").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u) << "no execute bit, nobody else";
#endif
}

TEST(ClipboardFilesUpload, RefusesBadListingsAndLimits) {
  scratch_t s;
  uploads_t uploads;
  EXPECT_TRUE(uploads.begin("c", {{"../evil", 1, false}}, s.root, ~0ull).id.empty());
  EXPECT_TRUE(uploads.begin("c", {{"a", 100, false}}, s.root, 100 + keep_free_bytes - 1).id.empty()) << "disk would fill";
  EXPECT_FALSE(uploads.begin("c", {{"a", 1, false}}, s.root, ~0ull).id.empty());
  EXPECT_FALSE(uploads.begin("c", {{"a", 1, false}}, s.root, ~0ull).id.empty());
  EXPECT_TRUE(uploads.begin("c", {{"a", 1, false}}, s.root, ~0ull).id.empty()) << "third at once";
  EXPECT_FALSE(uploads.begin("other", {{"a", 1, false}}, s.root, ~0ull).id.empty()) << "a different client is not counted";
  // Nothing escaped the staging root.
  for (const auto &p : fs::recursive_directory_iterator(s.root)) {
    EXPECT_TRUE(p.path().string().starts_with(s.root.string()));
  }
}

#ifndef _WIN32
TEST(ClipboardFilesUpload, NeverWritesThroughALink) {
  scratch_t s;
  const auto outside = s.root / "outside.txt";
  put(outside, "original");
  const auto staging = s.root / "staging";
  uploads_t uploads;
  const auto begun = uploads.begin("c", {{"a.txt", 4, false}}, staging, ~0ull);
  ASSERT_FALSE(begun.id.empty());
  // Something planted a link where the file is about to be created.
  fs::create_symlink(outside, staging / begun.id / "a.txt");
  std::string why;
  EXPECT_FALSE(uploads.write("c", begun.id, 0, 0, "evil", &why));
  EXPECT_EQ(get(outside), "original");
}
#endif

TEST(ClipboardFilesUpload, AbortAndSweepClean) {
  scratch_t s;
  uploads_t uploads;
  auto begun = uploads.begin("c", {{"a", 1, false}}, s.root, ~0ull);
  ASSERT_FALSE(begun.id.empty());
  uploads.abort("someone-else", begun.id);
  EXPECT_TRUE(fs::exists(s.root / begun.id));
  uploads.abort("c", begun.id);
  EXPECT_FALSE(fs::exists(s.root / begun.id));

  // A pasted folder from yesterday goes; today's, and anything not ours, stays.
  const auto old_id = std::string(32, 'a');
  const auto new_id = std::string(32, 'b');
  fs::create_directories(s.root / old_id / "sub");
  put(s.root / old_id / "sub" / "f", "x");
  fs::create_directories(s.root / new_id);
  fs::create_directories(s.root / "Not ours");
  fs::last_write_time(s.root / "Not ours", fs::file_time_type::clock::now() - keep_staged - std::chrono::hours(1));
#ifndef _WIN32
  const auto precious = s.root / "precious";
  fs::create_directories(precious);
  put(precious / "keep.txt", "keep");
  fs::create_directory_symlink(precious, s.root / old_id / "link-out");
#endif
  fs::last_write_time(s.root / old_id, fs::file_time_type::clock::now() - keep_staged - std::chrono::hours(1));
  uploads.sweep(s.root);
  EXPECT_FALSE(fs::exists(s.root / old_id));
  EXPECT_TRUE(fs::exists(s.root / new_id));
  EXPECT_TRUE(fs::exists(s.root / "Not ours"));
#ifndef _WIN32
  EXPECT_EQ(get(precious / "keep.txt"), "keep") << "the sweep followed a link";
#endif
}

TEST(ClipboardFilesSnapshot, ListsCopiedItemsAndNothingElse) {
  scratch_t s;
  const auto src = s.root / "src";
  fs::create_directories(src / "folder" / "inner");
  put(src / "folder" / "a.txt", "aaaa");
  put(src / "folder" / "inner" / "b.txt", "bb");
  put(src / "single.txt", "single");
  const auto secret = s.root / "secret";
  fs::create_directories(secret);
  put(secret / "id_rsa", "secret");
#ifndef _WIN32
  fs::create_symlink(secret / "id_rsa", src / "folder" / "link-to-secret");
  fs::create_directory_symlink(secret, src / "folder" / "dir-link");
  ASSERT_EQ(mkfifo((src / "folder" / "fifo").c_str(), 0600), 0);
  fs::create_symlink(src / "single.txt", src / "copied-link");
#endif

  std::vector<fs::path> tops {src / "folder", src / "single.txt"};
#ifndef _WIN32
  tops.push_back(src / "copied-link");
#endif
  const auto snap = make_snapshot(tops, 42);
  EXPECT_EQ(snap.sequence, 42u);
  EXPECT_FALSE(snap.truncated);
  std::vector<std::string> paths;
  for (const auto &e : snap.entries) {
    paths.push_back(e.path);
  }
  // The copied link resolves to single.txt, already listed under that name.
  EXPECT_EQ(paths, (std::vector<std::string> {"folder", "folder/a.txt", "folder/inner", "folder/inner/b.txt", "single.txt"}));
  EXPECT_TRUE(valid_entries(snap.entries));

  std::string out, why;
  ASSERT_TRUE(read_chunk(snap, 1, 0, out, &why)) << why;
  EXPECT_EQ(out, "aaaa");
  ASSERT_TRUE(read_chunk(snap, 1, 2, out, &why)) << why;
  EXPECT_EQ(out, "aa");
  EXPECT_FALSE(read_chunk(snap, 1, 5, out, &why)) << "past the end";
  EXPECT_FALSE(read_chunk(snap, 0, 0, out, &why)) << "a folder";
  EXPECT_FALSE(read_chunk(snap, 99, 0, out, &why)) << "no such entry";

  // The listed file is replaced by another one under the same name.
  put(s.root / "replacement", "other");
  fs::rename(s.root / "replacement", src / "folder" / "a.txt");
  EXPECT_FALSE(read_chunk(snap, 1, 0, out, &why)) << "a swapped file was served";
#ifndef _WIN32
  fs::remove(src / "folder" / "inner" / "b.txt");
  fs::create_symlink(secret / "id_rsa", src / "folder" / "inner" / "b.txt");
  EXPECT_FALSE(read_chunk(snap, 3, 0, out, &why)) << "a file swapped for a link was followed";
#endif
}

TEST(ClipboardFilesSnapshot, ChunksAndLimits) {
  scratch_t s;
  const auto big = s.root / "big.bin";
  put(big, std::string(chunk_bytes + 10, 'x'));
  const auto snap = make_snapshot({big}, 1);
  std::string out;
  ASSERT_TRUE(read_chunk(snap, 0, 0, out));
  EXPECT_EQ(out.size(), chunk_bytes);
  ASSERT_TRUE(read_chunk(snap, 0, chunk_bytes, out));
  EXPECT_EQ(out.size(), 10u);

  const auto dir = s.root / "many";
  fs::create_directories(dir);
  for (std::size_t i = 0; i < max_entries + 5; ++i) {
    put(dir / ("f" + std::to_string(i)), "");
  }
  EXPECT_TRUE(make_snapshot({dir}, 2).truncated);
}

TEST(ClipboardFilesPaths, Utf8RoundTrip) {
  const std::string name = "d\xC3\xA9j\xC3\xA0 vu \xE6\x96\x87.txt";
  EXPECT_EQ(path_to_utf8(path_from_utf8(name)), name);
}
