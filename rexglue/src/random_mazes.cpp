// Random maze order for Championship mode (--pacman_random_mazes=true, off by default).
//
// Each mode's settings are a Shift-JIS CSV in data\ (Championship = gamedata1.bin: timemax 300, menu order). Its
// stage lists - "ステージN（開始、終了グリッド）,m_i_mapXX,a,b" rows, one list per playfield half (dualmap) - give
// the maze for each stage, and the game reads them by position after loading all gamedata files at startup.
// At launch the file is read here, every list keeps its first row (the opening maze) and has the rest shuffled
// as whole lines, and the SDK's file read filter (patch 0007) hands the game that version. Same length, same rows,
// new order each launch; the file on disk is never changed. Random runs use their own save data
// (userdata\random-mazes, set in pacman_app.h), so their scores stay separate from normal play.

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_BOOL(pacman_random_mazes, false, "Pac-Man",
                    "Shuffle Championship mode's maze order at each launch (the first maze stays); uses separate "
                    "save data (userdata\\random-mazes). Command line only");

// Experiment (2026-10-21): which part of a stage row picks the maze? rows = move whole rows (default),
// names = permute only the m_i_mapXX field, values = permute only the numbers after it.
REXCVAR_DEFINE_STRING(pacman_random_mazes_test, "rows", "Pac-Man",
                      "Random mazes experiment: rows | names | values");

namespace {

constexpr std::string_view ChampionshipFile = "gamedata1.bin";
std::vector<uint8_t> g_shuffled;  // the whole file, with the stage lists reordered

bool EndsWithFileName(const std::string& path, std::string_view name) {
  if (path.size() < name.size())
    return false;
  const std::string_view tail(path.data() + path.size() - name.size(), name.size());
  return std::equal(tail.begin(), tail.end(), name.begin(), name.end(),
                    [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }) &&
         (path.size() == name.size() || path[path.size() - name.size() - 1] == '\\' ||
          path[path.size() - name.size() - 1] == '/');
}

void ReadFilter(const rex::filesystem::Entry& entry, size_t offset, std::span<uint8_t> data) {
  if (g_shuffled.empty() || !EndsWithFileName(entry.path(), ChampionshipFile))
    return;
  if (offset > g_shuffled.size() || data.size() > g_shuffled.size() - offset)
    return;  // not the file we prepared (size changed?): leave it alone
  std::memcpy(data.data(), g_shuffled.data() + offset, data.size());
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("random mazes: game read {} ({} bytes at offset {}) - shuffled version served", entry.path(),
                data.size(), offset);
  }
}

// Stage rows: "...（開始、終了グリッド）,m_i_map..." (the label in Shift-JIS).
bool IsStageRow(const std::string& line) {
  static const std::string label = "\x81\x69\x8A\x4A\x8E\x6E\x81\x41\x8F\x49\x97\xB9\x83\x4F\x83\x8A\x83\x62\x83\x68\x81\x6A";
  return line.find(label + ",m_i_map") != std::string::npos;
}

std::string MapName(const std::string& line) {
  const size_t start = line.find(",m_i_map");
  const size_t end = line.find(',', start + 1);
  return line.substr(start + 8, end - start - 8);
}

}  // namespace

// Called at startup (pacman_app.h, only when random mazes are on - decided together with the separate save
// folder) with the game data folder. Logs what it did.
void RandomMazesSetup(const std::filesystem::path& game_data_root) {
  std::ifstream in(game_data_root / "data" / std::string(ChampionshipFile), std::ios::binary);
  if (!in) {
    REXLOG_WARN("random mazes: can't read data\\{}", ChampionshipFile);
    return;
  }
  const std::string original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // Split into lines, keeping each line's own terminator.
  std::vector<std::string> lines;
  for (size_t pos = 0; pos < original.size();) {
    size_t end = original.find('\n', pos);
    end = end == std::string::npos ? original.size() : end + 1;
    lines.push_back(original.substr(pos, end - pos));
    pos = end;
  }

  std::random_device seed;
  std::mt19937 random(seed());
  int lists = 0;
  for (size_t i = 0; i < lines.size();) {
    if (!IsStageRow(lines[i])) {
      i++;
      continue;
    }
    size_t end = i;
    while (end < lines.size() && IsStageRow(lines[end]))
      end++;
    if (end - i > 2) {
      // Keep the first stage, shuffle the rest. Rows only move within this list, so its byte length stays
      // the same; the stage label text moves with its row but the game reads by position.
      const std::string mode = REXCVAR_GET(pacman_random_mazes_test);
      if (mode == "names" || mode == "values") {
        // Split each row after its second comma (label,name | values) and permute one side only.
        std::vector<size_t> order(end - i - 1);
        for (size_t k = 0; k < order.size(); k++)
          order[k] = i + 1 + k;
        std::shuffle(order.begin(), order.end(), random);
        const std::vector<std::string> before(lines.begin() + i + 1, lines.begin() + end);
        auto split = [](const std::string& line) { return line.find(',', line.find(',') + 1); };
        for (size_t k = 0; k < order.size(); k++) {
          const std::string& own = before[k];
          const std::string& other = before[order[k] - i - 1];
          const size_t own_split = split(own), other_split = split(other);
          if (mode == "names") {
            const size_t own_name = own.find(','), other_name = other.find(',');
            lines[i + 1 + k] = own.substr(0, own_name) + other.substr(other_name, other_split - other_name) +
                               own.substr(own_split);
          } else {
            lines[i + 1 + k] = own.substr(0, own_split) + other.substr(other_split);
          }
        }
        REXLOG_INFO("random mazes: test mode {}", mode);
      } else {
        std::shuffle(lines.begin() + i + 1, lines.begin() + end, random);
      }
      std::string order;
      for (size_t k = i; k < end; k++)
        order += (k == i ? "" : " ") + MapName(lines[k]);
      REXLOG_INFO("random mazes: list {} order: {}", ++lists, order);
    }
    i = end;
  }
  std::string shuffled;
  for (const std::string& line : lines)
    shuffled += line;
  if (lists == 0 || shuffled.size() != original.size()) {
    REXLOG_WARN("random mazes: stage lists not found in data\\{}, playing the normal order", ChampionshipFile);
    return;
  }
  g_shuffled.assign(shuffled.begin(), shuffled.end());
  rex::filesystem::SetFileReadFilter(ReadFilter);
  REXLOG_INFO("random mazes: on ({} stage lists shuffled)", lists);
}
