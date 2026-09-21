# Multi-stage build ultra-leggera per PlatoLives Console (Linux ARM64 / x86_64)
FROM alpine:latest AS builder
RUN apk add --no-cache build-base cmake ninja

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target PlatoLives

# Immagine finale minimale da circa 8 MB
FROM alpine:latest
RUN apk add --no-cache libgcc

WORKDIR /app
COPY --from=builder /src/build/PlatoLives /app/PlatoLives

ENTRYPOINT ["/app/PlatoLives"]
