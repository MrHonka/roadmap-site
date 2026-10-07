# Спринт 2: multi-stage сборка C++ (сборка отдельно, запуск отдельно).
# Этап 1 — builder: тут есть g++/cmake, собираем бинарник.
FROM debian:bookworm-slim AS builder

# hadolint DL3008 у всех пакетов apt-get insatll указана конкретная версия. детерминированность сборок в будущем.
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++=4:12.2.0-3 cmake=3.25.1-1 make=4.3-4.1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ ./src/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ls -lh build/roadmap_server

# Этап 2 — runtime: компилятора тут НЕТ, только готовый бинарник + web + data.
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates=20250419~deb12u1 curl=7.88.1-10+deb12u15 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/roadmap_server /app/roadmap_server
COPY web/ /app/web/
COPY data/ /app/data/

ENV PORT=8080 \
    WEB_ROOT=/app/web \
    ROADMAP_FILE=/app/data/roadmap.json

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    # CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1 - hadolint DL3025 Docker оборачивает команду в /bin/sh -c, PID 1 становится shell, а не моя программа, и docker stop (SIGTERM) срабатывает криво. и || exit 1 - лишнее, т.к. curl и так возвращет ненулевой код в случае фейла
    CMD ["curl", "-fsS", "http://127.0.0.1:8080/healthz" ]

CMD ["/app/roadmap_server"]
