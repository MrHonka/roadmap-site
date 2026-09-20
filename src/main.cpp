// Roadmap server: cpp-httplib + nlohmann/json, локальное JSON-хранилище.
// Endpoints:
//   GET  /            -> web/index.html
//   GET  /api/roadmap -> JSON из data/roadmap.json
//   POST /api/roadmap -> валидация + атомарная запись в data/roadmap.json
//   GET  /healthz     -> {"status":"ok"} для Docker/K8s probes
// Env:
//   PORT         (default 8080)
//   WEB_ROOT     (default ./web, fallback ../web, .)
//   ROADMAP_FILE (default ./data/roadmap.json, fallback ../data/roadmap.json)
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
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

// Атомарная запись: tmp + rename. Держит бэкап .bak.
bool WriteFileAtomic(const std::string& path, const std::string& body,
                     std::string& err) {
  try {
    fs::path p(path);
    if (p.has_parent_path()) {
      std::error_code ec;
      fs::create_directories(p.parent_path(), ec);
    }
    const std::string tmp = path + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) {
        err = "cannot open tmp file";
        return false;
      }
      f << body;
      f.flush();
      if (!f) {
        err = "write failed";
        return false;
      }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
      err = "rename failed: " + ec.message();
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

}  // namespace

int main() {
  int port = std::atoi(EnvOr("PORT", "8080").c_str());
  if (port <= 0 || port > 65535) port = 8080;

  const std::string web_root = FirstExisting(
      {EnvOr("WEB_ROOT", "./web"), "./web", "../web", "."}, "./web");
  std::string roadmap_file = EnvOr("ROADMAP_FILE", "");
  if (roadmap_file.empty()) {
    roadmap_file =
        FirstExisting({"./data/roadmap.json", "../data/roadmap.json",
                       "data/roadmap.json"},
                      "./data/roadmap.json");
  }

  std::cout << "web_root=" << web_root << "\nroadmap_file=" << roadmap_file
            << "\nport=" << port << std::endl;

  httplib::Server svr;
  std::mutex store_mutex;

  // Healthcheck для Docker/K8s.
  svr.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"status":"ok"})", "application/json");
  });

  // Текущее состояние.
  svr.Get("/api/roadmap", [&](const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(store_mutex);
    const std::string body = ReadFile(roadmap_file);
    if (body.empty()) {
      res.status = 500;
      res.set_content(R"({"error":"cannot read roadmap file"})",
                      "application/json");
      return;
    }
    // Проверяем что файл — валидный JSON, иначе отдадим 500 а не мусор.
    try {
      (void)json::parse(body);
    } catch (const std::exception& e) {
      res.status = 500;
      res.set_content(
          json({{"error", std::string("invalid roadmap json: ") + e.what()}})
              .dump(),
          "application/json");
      return;
    }
    res.set_content(body, "application/json");
  });

  // Сохранение статусов/заметок.
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
    std::lock_guard<std::mutex> lock(store_mutex);
    std::string werr;
    if (!WriteFileAtomic(roadmap_file, j.dump(2, ' ', false) + "\n", werr)) {
      res.status = 500;
      res.set_content(json({{"error", werr}}).dump(), "application/json");
      return;
    }
    res.set_content(R"({"status":"saved"})", "application/json");
  });

  // Статика + index на /.
  svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
    const std::string index = web_root + "/index.html";
    const std::string body = ReadFile(index);
    if (body.empty()) {
      res.status = 500;
      res.set_content("index.html not found. Check WEB_ROOT.",
                      "text/plain; charset=utf-8");
      return;
    }
    res.set_content(body, "text/html; charset=utf-8");
  });

  // Остальная статика из web_root (если UI решишь разбить на файлы).
  svr.set_mount_point("/static", web_root);

  // Логи ошибок слушателя.
  const bool ok = svr.listen("0.0.0.0", port);
  if (!ok) {
    std::cerr << "listen on port " << port << " failed" << std::endl;
    return 1;
  }
  return 0;
}
