# AGENTS.md — roadmap-site (учебный стенд Kubena)

## Кто и где
- Ученик: DevOps с нуля, русский язык, объяснять просто, по шагам, без пропусков.
- Хост: Windows-ПК (основной, тут opencode). Сервер: Kubena (Ubuntu Server, второй ПК).
- Роутер: Mikrotik hap ac3 (была перепрошивка, IP уезжал — решено static lease).
- Репозиторий кода: https://github.com/MrHonka/roadmap-site (ветка master).
- Учебный план: https://github.com/MrHonka/eduvjaju (roadmap спринты 1-6).

## Проект
- C++ HTTP-сервер (cpp-httplib + nlohmann/json): `src/main.cpp`.
- API: `GET /` (web), `GET /api/roadmap`, `POST /api/roadmap`, `GET /healthz`.
- Хранилище сейчас: JSON `./data/roadmap.json` (bind `./data:/app/data`). Postgres в compose поднят, но код его ЕЩЁ НЕ использует.
- `Dockerfile`: multi-stage (builder debian + g++/cmake → runtime debian-slim + curl).
- `docker-compose.yml`: `app` (8080) + `db` (postgres:16-alpine, bind `/mnt/k8s-storage/pgdata`), healthchecks, `depends_on: service_healthy`, `restart: unless-stopped`. Старый named volume `roadmap_pgdata` удален.
- Docker на Kubena: v29.8.1 (новый формат `docker images`: IMAGE / ID / DISK USAGE / CONTENT SIZE / EXTRA U=InUse).

## Что уже пройдено (не объяснять заново)
- Спринт 1 done: Ubuntu, static lease, SSH по ключам, known_hosts (перезапись при смене IP понятна).
- Git: init/add/commit/push в roadmap-site, user.name/email настроены. `src refspec master does not match any` = забыт commit.
- SSH: `ssh.service=inactive + ssh.socket=listening` = норма (socket-активация), не чинить.
- Docker база: образ (шаблон на диске) vs контейнер (живой процесс). `docker ps` = всё на хосте, `docker compose ps` = только проект. Порты `лево:право` = хост:контейнер. `WORKDIR /app` → `exec` стартует в /app. Bind vs named volume. `/var/lib/docker` только через sudo. `up -d` применяет yml (пересоздает), `restart` — нет.
- Лабы done: смена порта 8080→8081, `exec app sh` + `ls/cat`, сайт сохраняет галочки в JSON по кнопке Сохранить.
- Лабы done: (3) роняем db и смотрим depends_on/logs, (4) перенос pgdata на /mnt/k8s-storage.

## Текущий этап
- Спринт 2 в процессе. Осталось: (5) hadolint + trivy + ldd.
- Дальше по плану: Postgres-драйвер в C++ вместо JSON, затем Спринт 3 (Ansible).

## Как отвечать
- Команды разделять: «на Windows» vs «на Kubena по SSH».
- После каждого шага — что проверить (curl/ps/logs) и какой вывод ждать.
