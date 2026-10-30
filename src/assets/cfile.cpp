#include "assets/cfile.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

#include "core/byte_reader.h"
#include "core/error.h"

namespace oa {

namespace {

constexpr std::size_t kNameSize = 50;

// Returns `s` upper-cased with '/' as '\\', the key of a DOS path.
std::string path_key(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = c == '/' ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

// Returns the lines of `COB.CFG`, without line ends and blanks.
std::vector<std::string> cfg_lines(std::span<const std::uint8_t> data) {
  std::vector<std::string> out;
  std::string line;
  const auto flush = [&] {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.pop_back();
    }
    if (!line.empty()) {
      out.push_back(line);
    }
    line.clear();
  };
  for (const std::uint8_t b : data) {
    if (b == '\n' || b == 0x1a) {
      flush();
    } else {
      line += static_cast<char>(b);
    }
  }
  flush();
  return out;
}

}  // namespace

std::vector<CobMember> parse_cob_directory(std::span<const std::uint8_t> data, std::uint64_t file_size,
                                           std::size_t archive, std::string_view context) {
  ByteReader r(data, context);
  const std::uint32_t count = r.u32();
  const std::uint64_t data_start = 4 + (kNameSize + 4) * std::uint64_t{count};
  if (data_start > file_size) {
    r.fail(std::format("directory of {} members exceeds the file", count));
  }
  std::vector<CobMember> members(count);
  for (CobMember& m : members) {
    m.name = r.fixed_string(kNameSize);
    m.archive = archive;
  }
  for (CobMember& m : members) {
    m.offset = r.u32();
  }
  for (std::size_t i = 0; i < members.size(); ++i) {
    const std::uint64_t end = i + 1 < members.size() ? members[i + 1].offset : file_size;
    if (members[i].offset < data_start || members[i].offset > end || end > file_size) {
      r.fail(std::format("member {} has bad bounds 0x{:x}..0x{:x}", members[i].name, members[i].offset, end));
    }
    members[i].size = static_cast<std::uint32_t>(end - members[i].offset);
  }
  return members;
}

CobDirectory::CobDirectory(const Install& install) {
  for (const std::string& name : cfg_lines(install.read(Install::kMarkerFile))) {
    if (archives_.size() == kMaxArchives) {
      throw DataError(std::format("COB.CFG lists more than {} archives", kMaxArchives));
    }
    const std::optional<std::filesystem::path> path = install.find(name);
    if (!path) {
      throw DataError(std::format("{}: no archive {} (COB.CFG)", install.root().string(), name));
    }
    std::ifstream in(*path, std::ios::binary | std::ios::ate);
    if (!in) {
      throw DataError(std::format("cannot open {}", path->string()));
    }
    const auto size = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0);
    std::uint8_t count_bytes[4] = {};
    in.read(reinterpret_cast<char*>(count_bytes), 4);
    const std::uint32_t count = read_u32le(count_bytes, 0);
    const std::uint64_t dir_size = std::min<std::uint64_t>(4 + (kNameSize + 4) * std::uint64_t{count}, size);
    std::vector<std::uint8_t> dir(dir_size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(dir.data()), static_cast<std::streamsize>(dir.size()));
    if (!in) {
      throw DataError(std::format("cannot read the directory of {}", path->string()));
    }
    std::vector<CobMember> members = parse_cob_directory(dir, size, archives_.size(), name);
    if (members_.size() + members.size() > kMaxMembers) {
      throw DataError(std::format("the archives hold more than {} members", kMaxMembers));
    }
    members_.insert(members_.end(), members.begin(), members.end());
    archives_.push_back(*path);
  }
}

const CobMember* CobDirectory::find(std::string_view dos_path) const {
  const std::string key = path_key(dos_path);
  const auto it = std::ranges::find_if(members_, [&](const CobMember& m) { return path_key(m.name) == key; });
  return it == members_.end() ? nullptr : &*it;
}

std::vector<std::uint8_t> CobDirectory::read(const CobMember& member) const {
  const std::filesystem::path& path = archives_.at(member.archive);
  std::ifstream in(path, std::ios::binary);
  std::vector<std::uint8_t> data(member.size);
  in.seekg(member.offset);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!in) {
    throw DataError(std::format("cannot read {} from {}", member.name, path.string()));
  }
  return data;
}

GameFiles::GameFiles(Install install, Language language)
    : install_(std::move(install)), directory_(install_), translation_(language) {}

std::optional<std::vector<std::uint8_t>> GameFiles::try_load(std::string_view dos_path) const {
  if (const CobMember* m = directory_.find(dos_path)) {
    return directory_.read(*m);
  }
  return install_.try_read(dos_path);
}

std::vector<std::uint8_t> GameFiles::load(std::string_view dos_path) const {
  auto data = try_load(dos_path);
  if (!data) {
    throw DataError(std::format("no game file {}", dos_path));
  }
  return std::move(*data);
}

}  // namespace oa
