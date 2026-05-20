# Shared C++ fixture generator. Sourced by gcc_compile.sh and
# clang_compile.sh so both compilers see *identical* input and the
# manifest comparison reflects compiler-side differences rather than
# fixture variance.

# generate_cxx_fixture <out-path>
generate_cxx_fixture() {
  local out="$1"
  cat > "$out" <<'HEAD'
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

template <typename K, typename V>
struct Table {
  std::vector<std::pair<K, V>> rows;
  std::unordered_map<K, size_t> by_key;
  void insert(K k, V v) {
    by_key[k] = rows.size();
    rows.emplace_back(std::move(k), std::move(v));
  }
  std::optional<V> get(const K& k) const {
    auto it = by_key.find(k);
    if (it == by_key.end()) return std::nullopt;
    return rows[it->second].second;
  }
};

template <typename T>
struct Graph {
  std::unordered_map<T, std::vector<T>> adj;
  void edge(T u, T v) { adj[u].push_back(v); adj[v].push_back(u); }
  std::vector<T> bfs(T s) const {
    std::vector<T> order;
    std::unordered_set<T> seen{s};
    std::vector<T> q{s};
    while (!q.empty()) {
      T cur = std::move(q.back()); q.pop_back();
      order.push_back(cur);
      auto it = adj.find(cur);
      if (it == adj.end()) continue;
      for (const auto& n : it->second)
        if (seen.insert(n).second) q.push_back(n);
    }
    return order;
  }
};

template <typename... Ts>
struct Tagged {
  std::variant<Ts...> v;
  template <typename U> bool is() const { return std::holds_alternative<U>(v); }
};

HEAD
  local N="${CXX_FIXTURE_BLOCKS:-60}"
  local i
  for ((i = 0; i < N; ++i)); do
    cat >> "$out" <<INST
int run_${i}(int seed) {
  Table<std::string, std::vector<int>> t_${i};
  Graph<int> g_${i};
  for (int j = 0; j < 16; ++j) {
    t_${i}.insert("k${i}_" + std::to_string(j),
                  std::vector<int>{seed, seed + j, seed * j});
    g_${i}.edge(j, (j + ${i}) & 15);
  }
  auto bfs = g_${i}.bfs(0);
  Tagged<int, std::string, std::vector<int>> tag_${i}{std::string{"v${i}"}};
  int acc = static_cast<int>(bfs.size());
  for (const auto& [k, v] : t_${i}.rows)
    acc += static_cast<int>(v.size()) + static_cast<int>(k.size());
  if (tag_${i}.is<std::string>()) acc ^= ${i};
  return acc;
}
INST
  done
  echo "int main() { int s = 0;" >> "$out"
  for ((i = 0; i < N; ++i)); do
    echo "  s ^= run_${i}(${i});" >> "$out"
  done
  echo "  return s & 0xff; }" >> "$out"
}
