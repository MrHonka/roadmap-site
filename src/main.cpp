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
#include <cstdlib>
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

  if (!svr.listen("0.0.0.0", port)) {
    std::cerr << "listen on port " << port << " failed" << std::endl;
    return 1;
  }
  return 0;
}
