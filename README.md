# ARK

A C89 web server library, split into a platform layer and an app.

- **Platform layer** (`src/platform/<os>/`, built as `debug/server`) owns `main`,
  memory, sockets and the event loop. It loads the app at runtime, calls it once
  per tick, and carries out what the app asks for.
- **App** (everything else in `src/`, built as `debug/ArkSDK/lib/libark.so`) is
  freestanding C89: no libc, no OS calls. It only sees the structs in
  `include/ark/platform.h`.

`include/ark/platform.h` is the whole contract. Porting to another OS means
writing one new platform layer that fills in those structs; the app stays the same.

## Build and run

    ./build.sh          # build app and platform layer
    ./debug/server      # listens on 8080 (or pass a port)
    ./build.sh app      # rebuild only the app; a running server hot-reloads it
