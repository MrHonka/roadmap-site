# Спринт 2: multi-stage сборка C++ (сборка отдельно, запуск отдельно).
# Этап 1 — builder: тут есть g++/cmake, собираем бинарник.
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make \
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
    ca-certificates curl \
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
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

CMD ["/app/roadmap_server"]
