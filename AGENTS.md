# AGENTS.md — roadmap-site (учебный стенд Kubena)

## Кто и где
- Ученик: DevOps с нуля, русский язык, объяснять просто, по шагам, без пропусков.
- Хост: Windows-ПК (основной, тут opencode) — чисто клиент, WSL не ставим.
- Сервер: Kubena (Ubuntu Server, второй ПК) — на нём же ставим Ansible (control node = Kubena).
- Роутер: Mikrotik hap ac3 (была перепрошивка, IP уезжал — решено static lease).
- Репозиторий кода: https://github.com/MrHonka/roadmap-site (ветка master).
- Учебный план: https://github.com/MrHonka/eduvjaju (roadmap спринты 1-6).

## Проект
- C++ HTTP-сервер (cpp-httplib + nlohmann/json): `src/main.cpp`.
- API: `GET /` (web), `GET /api/roadmap`, `POST /api/roadmap`, `GET /healthz` (liveness), `GET /readyz` (readiness, SELECT 1).
- Хранилище: Postgres (таблицы `sprints` + `items`, транзакционный Save, seed из JSON при пустых таблицах). JSON-файл теперь только seed.
- `Dockerfile`: multi-stage (builder debian + g++/cmake + libpq-dev → runtime debian-slim + curl + libpq5), версии запинены.
- `docker-compose.yml`: `app` (8080, `DATABASE_URL` с хостом `db`) + `db` (postgres:16-alpine, bind `/mnt/k8s-storage/pgdata`), healthchecks, `depends_on: service_healthy`, `restart: unless-stopped`. Старый named volume `roadmap_pgdata` удален.
- Docker на Kubena: v29.8.1 (новый формат `docker images`: IMAGE / ID / DISK USAGE / CONTENT SIZE / EXTRA U=InUse).

## Что уже пройдено (не объяснять заново)
- Спринт 1 done: Ubuntu, static lease, SSH по ключам, known_hosts (перезапись при смене IP понятна).
- SSH-доступ: только через кастомный клиент, `ssh-add` выполняется в момент подключения. Пустой `ssh-add -l` и `Permission denied` из обычного PowerShell без `ssh-add` — норма, не чинить.
- Git: init/add/commit/push в roadmap-site, user.name/email настроены. `src refspec master does not match any` = забыт commit.
- SSH: `ssh.service=inactive + ssh.socket=listening` = норма (socket-активация), не чинить.
- Docker база: образ (шаблон на диске) vs контейнер (живой процесс). `docker ps` = всё на хосте, `docker compose ps` = только проект. Порты `лево:право` = хост:контейнер. `WORKDIR /app` → `exec` стартует в /app. Bind vs named volume. `/var/lib/docker` только через sudo. `up -d` применяет yml (пересоздает), `restart` — нет.
- Лабы done: смена порта 8080→8081, `exec app sh` + `ls/cat`, сайт сохраняет галочки в JSON по кнопке Сохранить.
- Лабы done: (3) роняем db и смотрим depends_on/logs, (4) перенос pgdata на /mnt/k8s-storage, (5) hadolint чисто + trivy-скан (340: принят) + ldd.
- Лабы done: (6) Postgres-драйвер (libpq): схема, seed, PQexecParams, один PGconn под мьютексом. Баги были: SELECT проверял как COMMAND_OK + double-free PQclear (ронял процесс, чинился restart-политикой).

## Текущий этап
- Спринт 2 done. Дальше: Спринт 3 (Ansible).
- Дальше по плану: Postgres-драйвер в C++ вместо JSON, затем Спринт 3 (Ansible).

## Как отвечать
- Команды разделять: «на Windows» vs «на Kubena по SSH».
- После каждого шага — что проверить (curl/ps/logs) и какой вывод ждать.
- Права ассистента: только папка проекта, сам на Kubena ничего не выполняет и интерактивный ssh не запускает. Только план + тексты файлов, все действия делает ученик.
