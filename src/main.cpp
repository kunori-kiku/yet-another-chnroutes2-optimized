// src/main.cpp
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <curl/curl.h>

#include <boost/multiprecision/cpp_int.hpp>

using u128 = boost::multiprecision::uint128_t;

static const u128 U128_MAX = ~u128(0);

struct Interval128 {
  u128 l;
  u128 r; // inclusive
};

struct Source {
  int ipver;              // 4 or 6
  std::string proto_name; // label written into outputs
  std::string url;
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* s = reinterpret_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

static bool http_get_to_string(const std::string& url, std::string& out, std::string& err) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl_easy_init failed";
    return false;
  }
  out.clear();

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cnroutes-aggregator/1.0");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    err = curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return false;
  }

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  if (code < 200 || code >= 300) {
    err = "HTTP " + std::to_string(code);
    return false;
  }
  return true;
}

static inline std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
  return s.substr(b, e - b);
}

static inline bool is_comment_or_empty(const std::string& line) {
  if (line.empty()) return true;
  if (line[0] == '#') return true;
  if (line.size() >= 2 && line[0] == '/' && line[1] == '/') return true;
  if (line[0] == ';') return true;
  return false;
}

static bool parse_cidr_ipv4(const std::string& cidr, Interval128& out) {
  auto pos = cidr.find('/');
  if (pos == std::string::npos) return false;
  std::string ip = cidr.substr(0, pos);
  std::string ps = cidr.substr(pos + 1);
  int prefix = -1;
  try { prefix = std::stoi(ps); } catch (...) { return false; }
  if (prefix < 0 || prefix > 32) return false;

  in_addr a{};
  if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
  uint32_t x = ntohl(a.s_addr);

  uint32_t mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
  uint32_t net = x & mask;
  uint32_t bcast = net | (~mask);

  out.l = u128(net);
  out.r = u128(bcast);
  return true;
}

static u128 bytes_to_u128_be(const uint8_t b[16]) {
  u128 v = 0;
  for (int i = 0; i < 16; i++) {
    v <<= 8;
    v |= u128(b[i]);
  }
  return v;
}

static void u128_to_bytes_be(u128 v, uint8_t out[16]) {
  for (int i = 15; i >= 0; i--) {
    out[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

static bool parse_cidr_ipv6(const std::string& cidr, Interval128& out) {
  auto pos = cidr.find('/');
  if (pos == std::string::npos) return false;
  std::string ip = cidr.substr(0, pos);
  std::string ps = cidr.substr(pos + 1);
  int prefix = -1;
  try { prefix = std::stoi(ps); } catch (...) { return false; }
  if (prefix < 0 || prefix > 128) return false;

  in6_addr a6{};
  if (inet_pton(AF_INET6, ip.c_str(), &a6) != 1) return false;
  u128 x = bytes_to_u128_be(reinterpret_cast<uint8_t*>(a6.s6_addr));

  u128 mask = 0;
  if (prefix == 0) {
    mask = 0;
  } else if (prefix == 128) {
    mask = U128_MAX;
  } else {
    mask = (U128_MAX << (128 - prefix));
  }

  u128 net = x & mask;
  u128 hi = net | (~mask);

  out.l = net;
  out.r = hi;
  return true;
}

static std::string u128_to_ipv6(u128 v) {
  uint8_t b[16];
  u128_to_bytes_be(v, b);
  char buf[INET6_ADDRSTRLEN];
  in6_addr a6{};
  std::memcpy(a6.s6_addr, b, 16);
  if (!inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) return "::";
  return std::string(buf);
}

static std::string u32_to_ipv4(uint32_t v) {
  in_addr a{};
  a.s_addr = htonl(v);
  char buf[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &a, buf, sizeof(buf))) return "0.0.0.0";
  return std::string(buf);
}

static std::vector<Interval128> union_normalize(std::vector<Interval128> xs) {
  if (xs.empty()) return {};
  std::sort(xs.begin(), xs.end(), [](const Interval128& a, const Interval128& b) {
    if (a.l != b.l) return a.l < b.l;
    return a.r < b.r;
  });

  std::vector<Interval128> out;
  Interval128 cur = xs[0];

  for (size_t i = 1; i < xs.size(); i++) {
    const auto& nx = xs[i];

    // merge if overlap or adjacent (careful about overflow when cur.r == U128_MAX)
    bool adjacent = (cur.r != U128_MAX && nx.l == cur.r + 1);
    bool overlap = (nx.l <= cur.r);

    if (overlap || adjacent) {
      if (nx.r > cur.r) cur.r = nx.r;
    } else {
      out.push_back(cur);
      cur = nx;
    }
  }
  out.push_back(cur);
  return out;
}

static inline u128 floor_pow2(u128 x) {
  // largest power of two <= x, x > 0
  unsigned msb = boost::multiprecision::msb(x);
  return u128(1) << msb;
}

static std::vector<std::pair<u128, int>> interval_to_cidrs_v6(u128 l, u128 r) {
  std::vector<std::pair<u128, int>> out;

  // full range special-case: ::/0
  if (l == 0 && r == U128_MAX) {
    out.push_back({0, 0});
    return out;
  }

  while (l <= r) {
    u128 remaining = r - l + 1; // safe here because not full-range

    // alignment: largest block starting at l that is power-of-two sized
    unsigned tz = (l == 0) ? 128u : boost::multiprecision::lsb(l);
    u128 max_align = (tz == 128u) ? (u128(1) << 127) << 1 /* 2^128 not representable; won't be used here */ : (u128(1) << tz);

    u128 max_len = floor_pow2(remaining);
    u128 block = (max_align < max_len) ? max_align : max_len;

    unsigned block_msb = boost::multiprecision::msb(block);
    int prefix = 128 - static_cast<int>(block_msb);

    // For block=1 => msb=0 => prefix=128 OK
    out.push_back({l, prefix});
    l += block;
  }
  return out;
}

static std::vector<std::pair<uint32_t, int>> interval_to_cidrs_v4(uint32_t l, uint32_t r) {
  std::vector<std::pair<uint32_t, int>> out;

  uint64_t L = l, R = r;
  while (L <= R) {
    uint64_t remaining = R - L + 1;

    uint64_t max_align;
    if (L == 0) {
      // alignment could be 2^32, but cap by remaining via max_len anyway
      max_align = (1ULL << 32);
    } else {
      max_align = (L & (~L + 1)); // lowbit
    }

    uint64_t max_len = 1;
    while ((max_len << 1) <= remaining) max_len <<= 1;

    uint64_t block = std::min(max_align, max_len);

    int log2 = 0;
    while ((1ULL << log2) < block) log2++;
    int prefix = 32 - log2;

    out.push_back({static_cast<uint32_t>(L), prefix});
    L += block;
  }
  return out;
}

static void write_txt(const std::string& path,
                      int ipver,
                      const std::vector<std::pair<u128, int>>& cidrs_v6,
                      const std::vector<std::pair<uint32_t, int>>& cidrs_v4,
                      const std::vector<Source>& sources,
                      const std::string& generated_at) {
  std::ofstream f(path, std::ios::out | std::ios::trunc);
  f << "# generated_at=" << generated_at << "\n";
  for (const auto& s : sources) {
    if (s.ipver == ipver) f << "# source[" << s.proto_name << "]=" << s.url << "\n";
  }
  if (ipver == 4) {
    f << "# count=" << cidrs_v4.size() << "\n";
    for (auto [addr, p] : cidrs_v4) {
      f << u32_to_ipv4(addr) << "/" << p << "\n";
    }
  } else {
    f << "# count=" << cidrs_v6.size() << "\n";
    for (auto [addr, p] : cidrs_v6) {
      f << u128_to_ipv6(addr) << "/" << p << "\n";
    }
  }
}

static void write_set_only_nft(const std::string& path,
                               int ipver,
                               const std::string& set_name,
                               const std::vector<std::pair<u128, int>>& cidrs_v6,
                               const std::vector<std::pair<uint32_t, int>>& cidrs_v4,
                               const std::vector<Source>& sources,
                               const std::string& generated_at) {
  std::ofstream f(path, std::ios::out | std::ios::trunc);

  f << "# generated_at=" << generated_at << "\n";
  for (const auto& s : sources) {
    if (s.ipver == ipver) f << "# source[" << s.proto_name << "]=" << s.url << "\n";
  }

  f << "set " << set_name << " {\n";
  f << "  type " << (ipver == 4 ? "ipv4_addr" : "ipv6_addr") << ";\n";
  f << "  flags interval;\n";
  f << "  elements = {\n";

  if (ipver == 4) {
    for (size_t i = 0; i < cidrs_v4.size(); i++) {
      auto [addr, p] = cidrs_v4[i];
      f << "    " << u32_to_ipv4(addr) << "/" << p
        << (i + 1 == cidrs_v4.size() ? "\n" : ",\n");
    }
  } else {
    for (size_t i = 0; i < cidrs_v6.size(); i++) {
      auto [addr, p] = cidrs_v6[i];
      f << "    " << u128_to_ipv6(addr) << "/" << p
        << (i + 1 == cidrs_v6.size() ? "\n" : ",\n");
    }
  }

  f << "  }\n";
  f << "}\n";
}

static std::string now_iso8601_utc() {
  using namespace std::chrono;
  auto t = system_clock::now();
  std::time_t tt = system_clock::to_time_t(t);
  std::tm gmt{};
  gmtime_r(&tt, &gmt);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
  return std::string(buf);
}

static void usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " \\\n"
      << "    --v4-url <url> --v4-proto <name> \\\n"
      << "    --v6-url <url> --v6-proto <name> \\\n"
      << "    --out-dir <dir>\n";
}

int main(int argc, char** argv) {
  std::string v4_url = "https://chnroutes2.cdn.skk.moe/chnroutes.txt"; // from SukkaW/chnroutes2-optimized README :contentReference[oaicite:4]{index=4}
  std::string v4_proto = "chnroutes2-optimized";
  std::string v6_url = "https://ruleset.skk.moe/Clash/ip/china_ipv6.txt";
  std::string v6_proto = "ruleset.skk.moe";
  std::string out_dir = "dist";

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char* k) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Missing value after " << k << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--v4-url") v4_url = need("--v4-url");
    else if (a == "--v4-proto") v4_proto = need("--v4-proto");
    else if (a == "--v6-url") v6_url = need("--v6-url");
    else if (a == "--v6-proto") v6_proto = need("--v6-proto");
    else if (a == "--out-dir") out_dir = need("--out-dir");
    else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown arg: " << a << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  std::vector<Source> sources = {
      {4, v4_proto, v4_url},
      {6, v6_proto, v6_url},
  };

  std::vector<Interval128> iv4, iv6;

  for (const auto& s : sources) {
    std::string body, err;
    if (!http_get_to_string(s.url, body, err)) {
      std::cerr << "Fetch failed: " << s.url << " : " << err << "\n";
      return 1;
    }

    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
      line = trim(line);
      if (is_comment_or_empty(line)) continue;

      // strip inline comments after whitespace # or ;
      {
        size_t p = line.find(" #");
        if (p != std::string::npos) line = trim(line.substr(0, p));
        p = line.find(" ;");
        if (p != std::string::npos) line = trim(line.substr(0, p));
      }
      if (line.empty()) continue;

      Interval128 it{};
      bool ok = false;
      if (s.ipver == 4) ok = parse_cidr_ipv4(line, it);
      else ok = parse_cidr_ipv6(line, it);

      if (!ok) {
        // not fatal: just warn
        std::cerr << "WARN: skip unparsable line: " << line << "\n";
        continue;
      }
      if (s.ipver == 4) iv4.push_back(it);
      else iv6.push_back(it);
    }
  }

  auto n4 = union_normalize(std::move(iv4));
  auto n6 = union_normalize(std::move(iv6));

  std::vector<std::pair<uint32_t, int>> cidr4;
  for (auto& it : n4) {
    cidr4.reserve(cidr4.size() + 8);
    auto part = interval_to_cidrs_v4(static_cast<uint32_t>(it.l), static_cast<uint32_t>(it.r));
    cidr4.insert(cidr4.end(), part.begin(), part.end());
  }

  std::vector<std::pair<u128, int>> cidr6;
  for (auto& it : n6) {
    cidr6.reserve(cidr6.size() + 16);
    auto part = interval_to_cidrs_v6(it.l, it.r);
    cidr6.insert(cidr6.end(), part.begin(), part.end());
  }

  // write outputs
  std::string ts = now_iso8601_utc();

  // naive mkdir -p for GitHub runner: use system (portable enough for ubuntu-latest)
  std::string cmd = "mkdir -p " + out_dir;
  if (std::system(cmd.c_str()) != 0) {
    std::cerr << "Failed to create out dir: " << out_dir << "\n";
    return 1;
  }

  write_txt(out_dir + "/ip4.txt", 4, cidr6, cidr4, sources, ts);
  write_txt(out_dir + "/ip6.txt", 6, cidr6, cidr4, sources, ts);

  // IMPORTANT: separate tables to avoid duplicate table declaration if both included
  write_set_only_nft(out_dir + "/cn4.nft", 4, "cn4", cidr6, cidr4, sources, ts);
  write_set_only_nft(out_dir + "/cn6.nft", 6, "cn6", cidr6, cidr4, sources, ts);


  curl_global_cleanup();

  std::cerr << "OK: ip4=" << cidr4.size() << " ip6=" << cidr6.size() << " out=" << out_dir << "\n";
  return 0;
}
