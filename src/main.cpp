// Roadmap server: cpp-httplib + libpq (PostgreSQL).
// Endpoints (формат ответов НЕ менялся со времен JSON — фронт untouched):
//   GET  /            -> web/index.html
//   GET  /api/roadmap -> состояние из Postgres
//   POST /api/roadmap -> валидация + запись в Postgres одной транзакцией
//   GET  /healthz     -> {"status":"ok"} (liveness: процесс жив)
//   GET  /readyz      -> {"ready":true} + SELECT 1 (readiness: база отвечает)
// Хранилище: таблицы sprints + items. JSON-файл — только seed при пустых таблицах.
// Env:
//   PORT         (default 8080)
//   WEB_ROOT     (default ./web)
//   DATABASE_URL (default host=db, совпадает с compose; libpq понимает
//                 и PGHOST/PGUSER/PGPASSWORD вместо URL)
#include <libpq-fe.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdint>
#include <cctype>
#include <optional>

static int HammingDistance(uint64_t a, uint64_t b) {
  uint64_t x = a ^ b;
  int c = 0;
  while (x) { c += static_cast<int>(x & 1); x >>= 1; }
  return c;
}

// Average perceptual hash (aHash), 64-bit.
// Loads image as grayscale, samples an 8x8 grid, compares each sample to the
// mean. Similar images produce close hashes; compare via Hamming distance.
static bool ComputeAverageHash(const std::string& path, uint64_t& hash,
                               int& outW, int& outH) {
  int w = 0, h = 0, n = 0;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 1);
  if (!data) return false;
  outW = w;
  outH = h;
  const int k = 8;
  unsigned char samples[64];
  long long sum = 0;
  for (int y = 0; y < k; ++y) {
    for (int x = 0; x < k; ++x) {
      int sx = (x * w) / k;
      int sy = (y * h) / k;
      unsigned char v = data[sy * w + sx];
      samples[y * k + x] = v;
      sum += v;
    }
  }
  stbi_image_free(data);
  long long mean = sum / 64;
  uint64_t hsh = 0;
  for (int i = 0; i < 64; ++i) {
    hsh <<= 1;
    if (samples[i] >= static_cast<unsigned char>(mean)) hsh |= 1;
  }
  hash = hsh;
  return true;
}

static bool HasImageExt(const std::string& name) {
  std::string lower = name;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower.size() >= 4 &&
         (lower.compare(lower.size() - 4, 4, ".png") == 0 ||
          lower.compare(lower.size() - 4, 4, ".jpg") == 0 ||
          lower.compare(lower.size() - 5, 5, ".jpeg") == 0 ||
          lower.compare(lower.size() - 4, 4, ".bmp") == 0 ||
          lower.compare(lower.size() - 4, 4, ".gif") == 0 ||
          lower.compare(lower.size() - 4, 4, ".tga") == 0);
}


namespace {

std::string EnvOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

std::string FirstExisting(const std::vector<std::string>& candidates,
                          const std::string& fallback) {
  for (const auto& c : candidates) {
    std::error_code ec;
    if (fs::exists(c, ec)) return c;
  }
  return fallback;
}

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Минимальная валидация структуры, чтобы не сохранить мусор.
bool ValidRoadmap(const json& j, std::string& err) {
  if (!j.is_object() || !j.contains("sprints") || !j["sprints"].is_array()) {
    err = "root.sprints must be an array";
    return false;
  }
  for (const auto& s : j["sprints"]) {
    if (!s.is_object() || !s.contains("id") || !s.contains("title") ||
        !s.contains("items") || !s["items"].is_array()) {
      err = "each sprint needs id, title, items[]";
      return false;
    }
    for (const auto& it : s["items"]) {
      if (!it.is_object() || !it.contains("id") || !it.contains("text") ||
          !it.contains("done")) {
        err = "each item needs id, text, done";
        return false;
      }
    }
  }
  return true;
}

// --- Слой Postgres -------------------------------------------------------
// Одно соединение + мьютекс: httplib крутит пул потоков, а один PGconn
// параллельно использовать нельзя. Мьютекс уже знаком по JSON-версии.
class Store {
 public:
  explicit Store(std::string conninfo) : conninfo_(std::move(conninfo)) {}

  // Подключение с ретраями: compose хоть и ждет healthy db, в K8s
  // depends_on нет — учимся ждать базу кодом сразу.
  bool Connect() {
    for (int attempt = 1; attempt <= 30; ++attempt) {
      conn_ = PQconnectdb(conninfo_.c_str());
      if (conn_ && PQstatus(conn_) == CONNECTION_OK) return true;
      std::cerr << "db connect attempt " << attempt
                << " failed: " << (conn_ ? PQerrorMessage(conn_) : "(null)")
                << std::endl;
      if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
  }

  ~Store() {
    if (conn_) PQfinish(conn_);
  }

  bool EnsureSchema(std::string& err) {
    static const char* kSql =
        "CREATE TABLE IF NOT EXISTS sprints ("
        " id INTEGER PRIMARY KEY,"
        " title TEXT NOT NULL,"
        " task TEXT NOT NULL DEFAULT '',"
        " done BOOLEAN NOT NULL DEFAULT FALSE,"
        " notes TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS items ("
        " sprint_id INTEGER NOT NULL REFERENCES sprints(id)"
        " ON DELETE CASCADE,"
        " item_id TEXT NOT NULL,"
        " text TEXT NOT NULL,"
        " done BOOLEAN NOT NULL DEFAULT FALSE,"
        " PRIMARY KEY (sprint_id, item_id));";
    return ExecSimple(kSql, err);
  }

  // SELECT 1 для /readyz. SELECT возвращает строки (TUPLES_OK),
  // а не COMMAND_OK — проверять надо соответствующим чекером.
  bool Ping(std::string& err) {
    PGresult* r = PQexec(conn_, "SELECT 1;");
    const bool ok = CheckTuples(r, err);
    PQclear(r);
    return ok;
  }

  bool IsEmpty(bool& empty, std::string& err) {
    PGresult* r = PQexec(conn_, "SELECT COUNT(*) FROM sprints;");
    if (!CheckTuples(r, err)) return false;
    empty = (std::string(PQgetvalue(r, 0, 0)) == "0");
    PQclear(r);
    return true;
  }

  bool Load(json& out, std::string& err) {
    out = json({{"sprints", json::array()}});
    PGresult* sprints =
        PQexec(conn_,
               "SELECT id, title, task, done, notes FROM sprints ORDER BY id;");
    if (!CheckTuples(sprints, err)) {
      PQclear(sprints);
      return false;
    }
    PGresult* items = PQexec(
        conn_,
        "SELECT sprint_id, item_id, text, done FROM items ORDER BY sprint_id;");
    if (!CheckTuples(items, err)) {
      PQclear(sprints);
      PQclear(items);
      return false;
    }
    // Раскладываем пункты по спринтам в памяти (N маленькое — можно).
    for (int i = 0; i < PQntuples(sprints); ++i) {
      json s;
      s["id"] = std::atoi(PQgetvalue(sprints, i, 0));
      s["title"] = PQgetvalue(sprints, i, 1);
      s["task"] = PQgetvalue(sprints, i, 2);
      s["done"] = (std::string(PQgetvalue(sprints, i, 3)) == "t");
      s["notes"] = PQgetvalue(sprints, i, 4);
      s["items"] = json::array();
      out["sprints"].push_back(std::move(s));
    }
    for (int i = 0; i < PQntuples(items); ++i) {
      const int sid = std::atoi(PQgetvalue(items, i, 0));
      for (auto& s : out["sprints"]) {
        if (s["id"] == sid) {
          json it;
          it["id"] = PQgetvalue(items, i, 1);
          it["text"] = PQgetvalue(items, i, 2);
          it["done"] = (std::string(PQgetvalue(items, i, 3)) == "t");
          s["items"].push_back(std::move(it));
          break;
        }
      }
    }
    PQclear(sprints);
    PQclear(items);
    return true;
  }

  // Полная замена состояния одной транзакцией: либо всё, либо ничего.
  bool Save(const json& j, std::string& err) {
    if (!ExecSimple("BEGIN;", err)) return false;
    if (!ExecSimple("DELETE FROM sprints;", err)) {  // items чистятся CASCADE
      ExecSimple("ROLLBACK;", err);
      return false;
    }
    for (const auto& s : j["sprints"]) {
      const std::string sid = std::to_string(s["id"].get<int>());
      const std::string title = s["title"].get<std::string>();
      const std::string task =
          s.contains("task") ? s["task"].get<std::string>() : "";
      const char* sdone = s.value("done", false) ? "TRUE" : "FALSE";
      const std::string notes =
          s.contains("notes") ? s["notes"].get<std::string>() : "";
      // Параметры $1.. вместо подстановки в строку — защита от инъекций.
      const char* vals[] = {sid.c_str(), title.c_str(), task.c_str(), sdone,
                            notes.c_str()};
      PGresult* r = PQexecParams(
          conn_,
          "INSERT INTO sprints (id, title, task, done, notes)"
          " VALUES ($1::int, $2, $3, $4::boolean, $5);",
          5, nullptr, vals, nullptr, nullptr, 0);
      if (!CheckCommand(r, err)) {
        PQclear(r);
        ExecSimple("ROLLBACK;", err);
        return false;
      }
      PQclear(r);
      for (const auto& it : s["items"]) {
        const std::string text = it["text"].get<std::string>();
        const std::string iid = it["id"].get<std::string>();
        const char* idone = it.value("done", false) ? "TRUE" : "FALSE";
        const char* ivals[] = {sid.c_str(), iid.c_str(), text.c_str(), idone};
        PGresult* ir = PQexecParams(
            conn_,
            "INSERT INTO items (sprint_id, item_id, text, done)"
            " VALUES ($1::int, $2, $3, $4::boolean);",
            4, nullptr, ivals, nullptr, nullptr, 0);
        if (!CheckCommand(ir, err)) {
          PQclear(ir);
          ExecSimple("ROLLBACK;", err);
          return false;
        }
        PQclear(ir);
      }
    }
    return ExecSimple("COMMIT;", err);
  }

  std::mutex& Mutex() { return mutex_; }

 private:
  bool ExecSimple(const char* sql, std::string& err) {
    PGresult* r = PQexec(conn_, sql);
    const bool ok = CheckCommand(r, err);
    PQclear(r);
    return ok;
  }

  // Чекеры НЕ освобождают результат: владелец всегда caller,
  // иначе двойной PQclear (double-free) роняет процесс.
  bool CheckCommand(PGresult* r, std::string& err) {
    if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
      err = r ? PQresultErrorMessage(r) : std::string(PQerrorMessage(conn_));
      return false;
    }
    return true;
  }

  bool CheckTuples(PGresult* r, std::string& err) {
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      err = r ? PQresultErrorMessage(r) : std::string(PQerrorMessage(conn_));
      return false;
    }
    return true;
  }

  std::string conninfo_;
  std::string uploads_dir_;
  PGconn* conn_ = nullptr;
  std::mutex mutex_;
};

// --- Image comparison layer ------------------------------------------------
// Mirrors Image Comparer: folders -> galleries with stored perceptual hashes,
// comparison within one gallery or between two galleries, similarity threshold.
class CompareStore {
 public:
  struct ImageInfo {
    int id = 0;
    int gallery_id = 0;
    std::string filename;
    std::string stored_name;
    uint64_t hash = 0;
    int width = 0;
    int height = 0;
    int size_bytes = 0;
  };

  explicit CompareStore(std::string conninfo, std::string uploads_dir)
      : conninfo_(std::move(conninfo)), uploads_dir_(std::move(uploads_dir)) {}

  bool Connect() {
    for (int attempt = 1; attempt <= 30; ++attempt) {
      conn_ = PQconnectdb(conninfo_.c_str());
      if (conn_ && PQstatus(conn_) == CONNECTION_OK) return true;
      std::cerr << "compare db connect attempt " << attempt
                << " failed: " << (conn_ ? PQerrorMessage(conn_) : "(null)")
                << std::endl;
      if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
  }

  ~CompareStore() {
    if (conn_) PQfinish(conn_);
  }

  bool EnsureSchema(std::string& err) {
    static const char* kSql =
        "CREATE TABLE IF NOT EXISTS compare_galleries ("
        " id SERIAL PRIMARY KEY,"
        " name TEXT NOT NULL,"
        " created_at TIMESTAMP DEFAULT NOW());"
        "CREATE TABLE IF NOT EXISTS compare_images ("
        " id SERIAL PRIMARY KEY,"
        " gallery_id INTEGER NOT NULL REFERENCES compare_galleries(id)"
        " ON DELETE CASCADE,"
        " filename TEXT NOT NULL,"
        " stored_name TEXT NOT NULL,"
        " hash TEXT NOT NULL,"
        " width INTEGER NOT NULL DEFAULT 0,"
        " height INTEGER NOT NULL DEFAULT 0,"
        " size_bytes INTEGER NOT NULL DEFAULT 0,"
        " uploaded_at TIMESTAMP DEFAULT NOW());"
        "CREATE TABLE IF NOT EXISTS compare_runs ("
        " id SERIAL PRIMARY KEY,"
        " gallery_a_id INTEGER NOT NULL REFERENCES compare_galleries(id),"
        " gallery_b_id INTEGER REFERENCES compare_galleries(id),"
        " threshold INTEGER NOT NULL DEFAULT 0,"
        " created_at TIMESTAMP DEFAULT NOW());"
        "CREATE TABLE IF NOT EXISTS compare_results ("
        " id SERIAL PRIMARY KEY,"
        " run_id INTEGER NOT NULL REFERENCES compare_runs(id)"
        " ON DELETE CASCADE,"
        " image_a_id INTEGER NOT NULL REFERENCES compare_images(id),"
        " image_b_id INTEGER NOT NULL REFERENCES compare_images(id),"
        " hamming_distance INTEGER NOT NULL,"
        " similarity REAL NOT NULL);";
    return ExecSimple(kSql, err);
  }

  std::mutex& Mutex() { return mutex_; }

  bool ListGalleries(json& out, std::string& err) {
    out = json::array();
    PGresult* r =
        PQexec(conn_, "SELECT id, name, created_at FROM compare_galleries ORDER BY id;");
    if (!CheckTuples(r, err)) { PQclear(r); return false; }
    for (int i = 0; i < PQntuples(r); ++i) {
      json g;
      g["id"] = std::atoi(PQgetvalue(r, i, 0));
      g["name"] = PQgetvalue(r, i, 1);
      g["created_at"] = PQgetvalue(r, i, 2);
      out.push_back(std::move(g));
    }
    PQclear(r);
    return true;
  }

  bool CreateGallery(const std::string& name, int& id, std::string& err) {
    const char* vals[] = {name.c_str()};
    PGresult* r = PQexecParams(
        conn_,
        "INSERT INTO compare_galleries (name) VALUES ($1) RETURNING id;",
        1, nullptr, vals, nullptr, nullptr, 0);
    if (!CheckCommand(r, err)) { PQclear(r); return false; }
    id = std::atoi(PQgetvalue(r, 0, 0));
    PQclear(r);
    return true;
  }

  bool DeleteGallery(int id, std::string& err) {
    const char* vals[] = {std::to_string(id).c_str()};
    PGresult* r = PQexecParams(
        conn_,
        "DELETE FROM compare_galleries WHERE id=$1::int;",
        1, nullptr, vals, nullptr, nullptr, 0);
    if (!CheckCommand(r, err)) { PQclear(r); return false; }
    PQclear(r);
    return true;
  }

  bool ListImages(int gallery_id, std::vector<ImageInfo>& out, std::string& err) {
    out.clear();
    const char* vals[] = {std::to_string(gallery_id).c_str()};
    PGresult* r = PQexecParams(
        conn_,
        "SELECT id, filename, stored_name, hash, width, height, size_bytes "
        "FROM compare_images WHERE gallery_id=$1::int ORDER BY id;",
        1, nullptr, vals, nullptr, nullptr, 0);
    if (!CheckTuples(r, err)) { PQclear(r); return false; }
    for (int i = 0; i < PQntuples(r); ++i) {
      ImageInfo img;
      img.id = std::atoi(PQgetvalue(r, i, 0));
      img.filename = PQgetvalue(r, i, 1);
      img.stored_name = PQgetvalue(r, i, 2);
      try { img.hash = std::stoull(PQgetvalue(r, i, 3), nullptr, 16); } catch (...) { img.hash = 0; }
      img.width = std::atoi(PQgetvalue(r, i, 4));
      img.height = std::atoi(PQgetvalue(r, i, 5));
      img.size_bytes = std::atoi(PQgetvalue(r, i, 6));
      img.gallery_id = gallery_id;
      out.push_back(img);
    }
    PQclear(r);
    return true;
  }

  bool AddImage(int gallery_id, const std::string& filename,
                const std::string& stored_name, uint64_t hash, int w, int h,
                int size_bytes, int& out_id, std::string& err) {
    char hashbuf[32];
    std::snprintf(hashbuf, sizeof(hashbuf), "%016llx",
                  static_cast<unsigned long long>(hash));
    const char* vals[] = {
        std::to_string(gallery_id).c_str(), filename.c_str(),
        stored_name.c_str(), hashbuf, std::to_string(w).c_str(),
        std::to_string(h).c_str(), std::to_string(size_bytes).c_str()};
    PGresult* r = PQexecParams(
        conn_,
        "INSERT INTO compare_images (gallery_id, filename, stored_name, hash,"
        " width, height, size_bytes)"
        " VALUES ($1::int, $2, $3, $4, $5::int, $6::int, $7::int)"
        " RETURNING id;",
        7, nullptr, vals, nullptr, nullptr, 0);
    if (!CheckCommand(r, err)) { PQclear(r); return false; }
    out_id = std::atoi(PQgetvalue(r, 0, 0));
    PQclear(r);
    return true;
  }

  bool DeleteImage(int id, std::string& err) {
    const char* v[] = {std::to_string(id).c_str()};
    PGresult* r = PQexecParams(conn_,
        "SELECT stored_name FROM compare_images WHERE id=$1::int;",
        1, nullptr, v, nullptr, nullptr, 0);
    std::string stored;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
      stored = PQgetvalue(r, 0, 0);
    PQclear(r);
    const char* v2[] = {std::to_string(id).c_str()};
    PGresult* r2 = PQexecParams(conn_,
        "DELETE FROM compare_images WHERE id=$1::int;",
        1, nullptr, v2, nullptr, nullptr, 0);
    if (!CheckCommand(r2, err)) { PQclear(r2); return false; }
    PQclear(r2);
    if (!stored.empty()) {
      std::error_code ec;
      fs::remove(uploads_dir_ + "/" + stored, ec);
    }
    return true;
  }

  bool RunCompare(int gallery_a_id, std::optional<int> gallery_b_id, int threshold,
                  int& run_id, json& pairs, std::string& err) {
    std::vector<ImageInfo> images_a, images_b;
    if (!ListImages(gallery_a_id, images_a, err)) return false;
    if (gallery_b_id.has_value()) {
      if (!ListImages(*gallery_b_id, images_b, err)) return false;
    } else {
      images_b = images_a;
    }

    std::string name_a = "gallery " + std::to_string(gallery_a_id);
    std::string name_b = name_a;
    {
      const char* v[] = {std::to_string(gallery_a_id).c_str()};
      PGresult* r = PQexecParams(conn_, "SELECT name FROM compare_galleries WHERE id=$1::int;",
                                 1, nullptr, v, nullptr, nullptr, 0);
      if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
        name_a = PQgetvalue(r, 0, 0);
      PQclear(r);
    }
    if (gallery_b_id.has_value()) {
      const char* v[] = {std::to_string(*gallery_b_id).c_str()};
      PGresult* r = PQexecParams(conn_, "SELECT name FROM compare_galleries WHERE id=$1::int;",
                                 1, nullptr, v, nullptr, nullptr, 0);
      if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
        name_b = PQgetvalue(r, 0, 0);
      PQclear(r);
    }

    double min_sim = threshold / 100.0;
    json result_pairs = json::array();

    for (const auto& a : images_a) {
      for (const auto& b : images_b) {
        // within-gallery: skip self pairs and duplicates
        if (!gallery_b_id.has_value() && b.id <= a.id) continue;
        int dist = HammingDistance(a.hash, b.hash);
        double similarity = 1.0 - static_cast<double>(dist) / 64.0;
        if (similarity < min_sim) continue;

        json pair;
        pair["image_a"] = {
            {"id", a.id}, {"filename", a.filename},
            {"gallery_name", name_a},
            {"label", name_a + "/" + a.filename},
            {"url", "/uploads/" + a.stored_name}, {"width", a.width},
            {"height", a.height}, {"size_bytes", a.size_bytes}};
        pair["image_b"] = {
            {"id", b.id}, {"filename", b.filename},
            {"gallery_name", name_b},
            {"label", name_b + "/" + b.filename},
            {"url", "/uploads/" + b.stored_name}, {"width", b.width},
            {"height", b.height}, {"size_bytes", b.size_bytes}};
        pair["hamming_distance"] = dist;
        pair["similarity"] = std::round(similarity * 100.0) / 100.0;
        pair["similarity_pct"] = static_cast<int>(std::round(similarity * 100.0));
        result_pairs.push_back(std::move(pair));
      }
    }

    if (!ExecSimple("BEGIN;", err)) return false;
    std::string gallery_b_str;
    if (gallery_b_id.has_value()) gallery_b_str = std::to_string(*gallery_b_id);
    const char* rvals[] = {
        std::to_string(gallery_a_id).c_str(),
        gallery_b_id.has_value() ? gallery_b_str.c_str() : nullptr,
        std::to_string(threshold).c_str()};
    PGresult* rr = PQexecParams(
        conn_,
        "INSERT INTO compare_runs (gallery_a_id, gallery_b_id, threshold)"
        " VALUES ($1::int, $2::int, $3::int) RETURNING id;",
        3, nullptr, rvals, nullptr, nullptr, 0);
    if (!CheckCommand(rr, err)) { PQclear(rr); ExecSimple("ROLLBACK;", err); return false; }
    run_id = std::atoi(PQgetvalue(rr, 0, 0));
    PQclear(rr);

    for (const auto& p : result_pairs) {
      const char* avals[] = {
          std::to_string(run_id).c_str(),
          std::to_string(p["image_a"]["id"].get<int>()).c_str(),
          std::to_string(p["image_b"]["id"].get<int>()).c_str(),
          std::to_string(p["hamming_distance"].get<int>()).c_str(),
          std::to_string(p["similarity"].get<double>()).c_str()};
      PGresult* ir = PQexecParams(
          conn_,
          "INSERT INTO compare_results (run_id, image_a_id, image_b_id,"
          " hamming_distance, similarity)"
          " VALUES ($1::int, $2::int, $3::int, $4::int, $5::real);",
          5, nullptr, avals, nullptr, nullptr, 0);
      if (!CheckCommand(ir, err)) {
        PQclear(ir);
        ExecSimple("ROLLBACK;", err);
        return false;
      }
      PQclear(ir);
    }
    if (!ExecSimple("COMMIT;", err)) return false;

    pairs = std::move(result_pairs);
    return true;
  }

 private:
  bool ExecSimple(const char* sql, std::string& err) {
    PGresult* r = PQexec(conn_, sql);
    const bool ok = CheckCommand(r, err);
    PQclear(r);
    return ok;
  }

  bool CheckCommand(PGresult* r, std::string& err) {
    if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
      err = r ? PQresultErrorMessage(r) : std::string(PQerrorMessage(conn_));
      return false;
    }
    return true;
  }

  bool CheckTuples(PGresult* r, std::string& err) {
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      err = r ? PQresultErrorMessage(r) : std::string(PQerrorMessage(conn_));
      return false;
    }
    return true;
  }

  std::string conninfo_;
  std::string uploads_dir_;
  PGconn* conn_ = nullptr;
  std::mutex mutex_;
};

}  // namespace

int main() {
  int port = std::atoi(EnvOr("PORT", "8080").c_str());
  if (port <= 0 || port > 65535) port = 8080;

  const std::string web_root = FirstExisting(
      {EnvOr("WEB_ROOT", "./web"), "./web", "../web", "."}, "./web");
  // JSON теперь только seed для первого старта.
  const std::string seed_file =
      FirstExisting({"./data/roadmap.json", "../data/roadmap.json",
                     "data/roadmap.json", "./seed.json"},
                    "./data/roadmap.json");
  const std::string conninfo = EnvOr(
      "DATABASE_URL",
      "host=db port=5432 dbname=roadmap user=roadmap"
      " password=roadmap_pw_change_me connect_timeout=5");

  std::cout << "web_root=" << web_root << "\nport=" << port << std::endl;

  Store store(conninfo);
  if (!store.Connect()) {
    std::cerr << "cannot connect to postgres, exiting" << std::endl;
    return 1;
  }
  {
    std::lock_guard<std::mutex> lock(store.Mutex());
    std::string err;
    if (!store.EnsureSchema(err)) {
      std::cerr << "schema failed: " << err << std::endl;
      return 1;
    }
    bool empty = false;
    if (!store.IsEmpty(empty, err)) {
      std::cerr << "count failed: " << err << std::endl;
      return 1;
    }
    if (empty) {
      // Переезд: заливаем текущий JSON в пустые таблицы один раз.
      const std::string body = ReadFile(seed_file);
      if (!body.empty()) {
        try {
          json seed = json::parse(body);
          if (ValidRoadmap(seed, err)) {
            if (!store.Save(seed, err)) {
              std::cerr << "seed failed: " << err << std::endl;
              return 1;
            }
            std::cout << "seeded db from " << seed_file << std::endl;
          } else {
            std::cerr << "seed file invalid: " << err << std::endl;
          }
        } catch (const std::exception& e) {
          std::cerr << "seed parse failed: " << e.what() << std::endl;
        }
      } else {
        std::cout << "no seed file, starting with empty roadmap" << std::endl;
      }
    }
  }

  const std::string uploads_dir = EnvOr("UPLOADS_DIR", "./uploads");
  {
    std::error_code ec;
    fs::create_directories(uploads_dir, ec);
  }

  CompareStore cstore(conninfo, uploads_dir);
  if (!cstore.Connect()) {
    std::cerr << "cannot connect to postgres (compare), exiting" << std::endl;
    return 1;
  }
  {
    std::lock_guard<std::mutex> lock(cstore.Mutex());
    std::string err;
    if (!cstore.EnsureSchema(err)) {
      std::cerr << "compare schema failed: " << err << std::endl;
      return 1;
    }
  }

  httplib::Server svr;

  svr.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"status":"ok"})", "application/json");
  });

  svr.Get("/readyz", [&](const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(store.Mutex());
    std::string err;
    if (!store.Ping(err)) {
      res.status = 503;
      res.set_content(json({{"ready", false}, {"error", err}}).dump(),
                      "application/json");
      return;
    }
    res.set_content(R"({"ready":true})", "application/json");
  });

  svr.Get("/api/roadmap", [&](const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(store.Mutex());
    json out;
    std::string err;
    if (!store.Load(out, err)) {
      res.status = 500;
      res.set_content(json({{"error", err}}).dump(), "application/json");
      return;
    }
    res.set_content(out.dump(), "application/json");
  });

  svr.Post("/api/roadmap", [&](const httplib::Request& req,
                               httplib::Response& res) {
    json j;
    try {
      j = json::parse(req.body);
    } catch (const std::exception& e) {
      res.status = 400;
      res.set_content(
          json({{"error", std::string("invalid json: ") + e.what()}}).dump(),
          "application/json");
      return;
    }
    std::string verr;
    if (!ValidRoadmap(j, verr)) {
      res.status = 400;
      res.set_content(json({{"error", verr}}).dump(), "application/json");
      return;
    }
    std::lock_guard<std::mutex> lock(store.Mutex());
    std::string err;
    if (!store.Save(j, err)) {
      res.status = 500;
      res.set_content(json({{"error", err}}).dump(), "application/json");
      return;
    }
    res.set_content(R"({"status":"saved"})", "application/json");
  });

  // --- Image comparison API (Image Comparer-like: galleries + hashes) ---
  svr.Get("/api/compare/galleries", [&](const httplib::Request&,
                                        httplib::Response& res) {
    std::lock_guard<std::mutex> lock(cstore.Mutex());
    json out;
    std::string err;
    if (!cstore.ListGalleries(out, err)) {
      res.status = 500;
      res.set_content(json({{"error", err}}).dump(), "application/json");
      return;
    }
    res.set_content(json({{"galleries", out}}).dump(), "application/json");
  });

  svr.Post("/api/compare/galleries", [&](const httplib::Request& req,
                                         httplib::Response& res) {
    json j;
    try {
      j = json::parse(req.body);
    } catch (const std::exception& e) {
      res.status = 400;
      res.set_content(
          json({{"error", std::string("invalid json: ") + e.what()}}).dump(),
          "application/json");
      return;
    }
    if (!j.contains("name") || !j["name"].is_string() ||
        j["name"].get<std::string>().empty()) {
      res.status = 400;
      res.set_content(json({{"error", "name required"}}).dump(),
                      "application/json");
      return;
    }
    std::lock_guard<std::mutex> lock(cstore.Mutex());
    int id = 0;
    std::string err;
    if (!cstore.CreateGallery(j["name"].get<std::string>(), id, err)) {
      res.status = 500;
      res.set_content(json({{"error", err}}).dump(), "application/json");
      return;
    }
    res.set_content(json({{"id", id}, {"name", j["name"]}}).dump(),
                    "application/json");
  });

  svr.Delete(R"(/api/compare/galleries/([0-9]+))",
             [&](const httplib::Request& req, httplib::Response& res) {
               int id = std::atoi(req.matches[1].str().c_str());
               std::lock_guard<std::mutex> lock(cstore.Mutex());
               std::string err;
               if (!cstore.DeleteGallery(id, err)) {
                 res.status = 500;
                 res.set_content(json({{"error", err}}).dump(),
                                 "application/json");
                 return;
               }
               res.set_content(R"({"status":"deleted"})", "application/json");
             });

  svr.Post("/api/compare/upload", [&](const httplib::Request& req,
                                      httplib::Response& res) {
    int gallery_id = -1;
    std::string gallery_name;
    if (req.has_param("gallery_id")) {
      try {
        gallery_id = std::stoi(req.get_param_value("gallery_id"));
      } catch (...) {
        gallery_id = -1;
      }
    }
    if (req.has_param("gallery_name")) {
      gallery_name = req.get_param_value("gallery_name");
    }

    std::lock_guard<std::mutex> lock(cstore.Mutex());
    std::string err;
    if (gallery_id < 0) {
      if (gallery_name.empty()) {
        res.status = 400;
        res.set_content(
            json({{"error", "gallery_id or gallery_name required"}}).dump(),
            "application/json");
        return;
      }
      if (!cstore.CreateGallery(gallery_name, gallery_id, err)) {
        res.status = 500;
        res.set_content(json({{"error", err}}).dump(), "application/json");
        return;
      }
    }

    json uploaded = json::array();
    for (const auto& kv : req.files) {
      const auto& f = kv.second;
      if (!HasImageExt(f.filename)) continue;
      std::string stored = std::to_string(gallery_id) + "_" +
                           std::to_string(std::time(nullptr)) + "_" + f.filename;
      std::string path = uploads_dir + "/" + stored;
      {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(f.content.data(),
                  static_cast<std::streamsize>(f.content.size()));
      }
      uint64_t hash = 0;
      int w = 0, h = 0;
      bool hashed = ComputeAverageHash(path, hash, w, h);
      int img_id = 0;
      if (!cstore.AddImage(gallery_id, f.filename, stored, hashed ? hash : 0,
                           w, h, static_cast<int>(f.content.size()), img_id,
                           err)) {
        res.status = 500;
        res.set_content(json({{"error", err}}).dump(), "application/json");
        return;
      }
      json info;
      info["id"] = img_id;
      info["gallery_id"] = gallery_id;
      info["filename"] = f.filename;
      info["stored_name"] = stored;
      info["url"] = "/uploads/" + stored;
      info["width"] = w;
      info["height"] = h;
      info["hash"] = hashed;
      uploaded.push_back(std::move(info));
    }
    res.set_content(
        json({{"gallery_id", gallery_id}, {"uploaded", uploaded}}).dump(),
        "application/json");
  });

  svr.Get("/api/compare/images", [&](const httplib::Request& req,
                                     httplib::Response& res) {
    if (!req.has_param("gallery_id")) {
      res.status = 400;
      res.set_content(json({{"error", "gallery_id required"}}).dump(),
                      "application/json");
      return;
    }
    int gallery_id = std::atoi(req.get_param_value("gallery_id").c_str());
    std::lock_guard<std::mutex> lock(cstore.Mutex());
    std::vector<CompareStore::ImageInfo> imgs;
    std::string err;
    if (!cstore.ListImages(gallery_id, imgs, err)) {
      res.status = 500;
      res.set_content(json({{"error", err}}).dump(), "application/json");
      return;
    }
    json arr = json::array();
    for (const auto& img : imgs) {
      json o;
      o["id"] = img.id;
      o["gallery_id"] = img.gallery_id;
      o["filename"] = img.filename;
      o["stored_name"] = img.stored_name;
      o["url"] = "/uploads/" + img.stored_name;
      o["width"] = img.width;
      o["height"] = img.height;
      o["size_bytes"] = img.size_bytes;
      o["hash"] = img.hash != 0;
      arr.push_back(std::move(o));
    }
    res.set_content(json({{"images", arr}}).dump(), "application/json");
  });

  svr.Delete(R"(/api/compare/images/([0-9]+))",
             [&](const httplib::Request& req, httplib::Response& res) {
               int id = std::atoi(req.matches[1].str().c_str());
               std::lock_guard<std::mutex> lock(cstore.Mutex());
               std::string err;
               if (!cstore.DeleteImage(id, err)) {
                 res.status = 500;
                 res.set_content(json({{"error", err}}).dump(), "application/json");
                 return;
               }
               res.set_content(R"({"status":"deleted"})", "application/json");
             });

  svr.Post("/api/compare/compare", [&](const httplib::Request& req,
                                       httplib::Response& res) {
    json j;
    try {
      j = json::parse(req.body);
    } catch (const std::exception& e) {
      res.status = 400;
      res.set_content(
          json({{"error", std::string("invalid json: ") + e.what()}}).dump(),
          "application/json");
      return;
    }
    if (!j.contains("gallery_a_id") || !j["gallery_a_id"].is_number_integer()) {
      res.status = 400;
      res.set_content(json({{"error", "gallery_a_id required"}}).dump(),
                      "application/json");
      return;
    }
    int gallery_a_id = j["gallery_a_id"].get<int>();
    std::optional<int> gallery_b_id;
    if (j.contains("gallery_b_id") && j["gallery_b_id"].is_number_integer()) {
      gallery_b_id = j["gallery_b_id"].get<int>();
    }
    int threshold = j.value("threshold", 50);
    if (threshold < 0) threshold = 0;
    if (threshold > 100) threshold = 100;

    std::lock_guard<std::mutex> lock(cstore.Mutex());
    int run_id = 0;
    json pairs;
    std::string err;
    if (!cstore.RunCompare(gallery_a_id, gallery_b_id, threshold, run_id, pairs,
                           err)) {
      res.status = 500;
      res.set_content(json({{"error", err}}).dump(), "application/json");
      return;
    }
    res.set_content(
        json({{"run_id", run_id}, {"pairs", pairs}, {"count", pairs.size()}})
            .dump(),
        "application/json");
  });

  svr.Get(R"(/api/compare/run/([0-9]+))",
          [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(
                json({{"note", "results returned at compare time"}}).dump(),
                "application/json");
          });

  svr.Get("/compare", [&](const httplib::Request&, httplib::Response& res) {
    const std::string body = ReadFile(web_root + "/compare.html");
    if (body.empty()) {
      res.status = 500;
      res.set_content("compare.html not found. Check WEB_ROOT.",
                      "text/plain; charset=utf-8");
      return;
    }
    res.set_content(body, "text/html; charset=utf-8");
  });

  svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
    const std::string body = ReadFile(web_root + "/index.html");
    if (body.empty()) {
      res.status = 500;
      res.set_content("index.html not found. Check WEB_ROOT.",
                      "text/plain; charset=utf-8");
      return;
    }
    res.set_content(body, "text/html; charset=utf-8");
  });

  svr.set_mount_point("/static", web_root);
  svr.set_mount_point("/uploads", uploads_dir);

  if (!svr.listen("0.0.0.0", port)) {
    std::cerr << "listen on port " << port << " failed" << std::endl;
    return 1;
  }
  return 0;
}
