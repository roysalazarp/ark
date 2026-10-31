# ARK

A C89 web server library, split into a platform layer and an app.

- **Platform layer** (`src/platform/<os>/`, built as `server`) owns `main`,
  memory, sockets and the event loop. It loads the app at runtime, calls it once
  per tick, and carries out what the app asks for.
- **App** (everything else in `src/`, built as `ArkSDK/lib/libark.so`) is
  freestanding C89: no libc, no OS calls. It only sees the structs in
  `include/ark/platform.h`.

`include/ark/platform.h` is the whole contract. Porting to another OS means
writing one new platform layer that fills in those structs; the app stays the same.

## Build and run

    ./build.sh            # debug build (-O0) into debug/
    ./build.sh release    # optimized build (-O2) into release/
    ./debug/server        # listens on 8080 (or pass a port)
    ./build.sh app        # rebuild only the debug app; a running server hot-reloads it

Hot reload only works while the app's state structs keep the same layout.
After changing a struct stored in permanent memory, restart the server.

## Test and benchmark

    ./test/http_test.sh [--slow]   # HTTP behaviour; --slow adds the timeout tests
    ./bench/run.sh                 # wrk benchmark against an equivalent Node.js server
