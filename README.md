# isolate-host

A standalone, capability-driven JavaScript/Wasm worker host built on a pinned,
patched Cloudflare workerd. It is extracted from Sandstorm, but contains no
Sandstorm storage, account, package, or permissions implementation.

The host runs as a separate process. A trusted controller supplies worker bundles
and scoped service capabilities over Cap'n Proto RPC. No workerd or V8 C++ ABI is
exposed to consumers.

## Build and test

Linux x86-64 is the initial release target. Building requires Python 3.12+, Git,
patch, CMake 3.25+, a C++23-capable Clang, libc++ and libc++abi development files,
and network access for the pinned dependencies. `upstream.json` pins workerd and
Bazel; `scripts/toolchain.py` installs the Chromium Clang used by CI. The release
workflow uses Ubuntu 24.04 with libc++ 19.

```sh
python3 scripts/toolchain.py
export CC="$PWD/build/llvm/bin/clang"
export CXX="$PWD/build/llvm/bin/clang++"
python3 scripts/build.py --test \
  --bazel-option=--linkopt=/usr/lib/llvm-19/lib/libc++abi.a \
  --bazel-option=--host_linkopt=/usr/lib/llvm-19/lib/libc++abi.a
python3 scripts/package.py
```

Adjust the libc++abi archive path for your distribution. The host binary is
`build/bin/isolate-host`. `--source /path/to/workerd` accepts an existing checkout
at the pinned commit; only its committed contents enter the build. Use
`--client-capnp-source /path/to/capnproto` for a local checkout of the test client's
pinned Cap'n Proto revision (see `tests/CMakeLists.txt`). `--prepare-only` verifies
and applies the overlay without building. Build outputs live under `build/`.

Tests use a separate Cap'n Proto 1.x client against the workerd-side 2.x runtime.
They exercise bundle admission without sockets, inherited-FD lifetime, HTTP
request/response streaming, named service and native RPC
bindings, literal and self bindings, worker lifetime, invalid bundles, CPU and
memory limits, idle eviction, and thread cleanup.

## Process and protocol

```sh
isolate-host --version
isolate-host --control-fd 3
# Development mode: listen on a private Unix socket path instead.
isolate-host /tmp/private-directory/control.sock
```

The inherited FD must be a connected Unix stream socket. It carries two-party
Cap'n Proto RPC with `IsolateHost` as the server bootstrap. The host exits when
that controller disconnects. Standard output carries structured worker logs;
standard error carries diagnostics. The runtime currently also needs a writable
private `/tmp` for its internal loader socket.

The parent owns OS confinement, resource limits, process lifetime, and assignment
of trust domains to processes. This executable does not install a namespace or
seccomp sandbox. Preserve that separation when integrating it into a supervisor.

The public schemas are in `protocol/`:

* `getInfo()` reports protocol version 1 and worker-source format version 1.
* `startWorker(workerId, services, workerSource)` creates or retains a worker.
  IDs are opaque, nonempty, at most 256 bytes, and never used as paths. Starting
  an already-running ID retains its original source and capabilities; call
  `stop()` before replacing it.
* `workerSource` is a **packed** `IsolateWorkerSource` message. It includes the
  main module, compatibility date/flags, modules, and named bindings.
* Bindings support text, bytes, JSON, HTTP services, native Cap'n Proto
  bootstraps, and a worker-local `service = "main"` Fetcher. Service names are
  resolved through the worker's `IsolateBindingServices` capability. Native
  RPC bindings expose `open()`, whose channel has `send(bytes)`, asynchronous
  `receive()`, and `close()`; frames are uncompressed Cap'n Proto stream messages.
* `HostedIsolate` provides `keepAlive()`, `stop()`, and `getHttpService()`.
  Possession of that capability authorizes HTTP ingress; knowledge of an ID does
  not. HTTP uses `http-over-capnp`'s `startRequest` (LEVEL_1) wire contract,
  including streaming bodies and WebSockets. A Cap'n Proto 2.x client must supply
  a LEVEL_1 adapter; its stock HTTP factory only supports LEVEL_2.

Supply only capabilities already confined to the intended worker. The native
bridge is opaque to the host, and does not transmit file descriptors. Module
names are passed through without Sandstorm-specific aliases. Global outbound
network access is not configured; workers access supplied service bindings.

Current fixed policy includes 64 MiB old/16 MiB young V8 heap limits, 16 MiB
worker bundles, 64 subrequests, a 250 ms JavaScript watchdog per request, a 5 s
startup watchdog, and 180 s idle eviction. The watchdog is elapsed-time based.
`ISOLATE_HOST_IDLE_TIMEOUT_MS` overrides the idle timeout (1 ms–24 hours),
primarily for tests. See the source for all admission and message-size limits.

## Releases

Tags matching `v<version>` in `upstream.json` build, test, and publish releases to
`https://github.com/mnutt/isolate-host`. No release is published by local scripts.
The workflow publishes:

* `isolate-host-<version>-linux-x86_64.tar.xz`: executable, schemas, licenses,
  and a manifest recording upstream revision, patch/binary hashes, protocol
  versions, dynamic dependencies, and the binary's minimum glibc version.
* `SHA256SUMS` and `manifest.json`.

Consumers pin the archive URL **and its SHA-256**, install `bin/isolate-host`,
and vendor the accompanying schemas. Their own Cap'n Proto compiler generates
client bindings; they do not need Bazel, workerd, V8, or this compiler toolchain.
The release binary is dynamically linked to the Linux C runtime. Check the
manifest's glibc requirement and test under the consumer's actual sandbox.

The protocol is new and intentionally incompatible with Sandstorm's private
`startGrain` interface. This is not a drop-in binary replacement until the
consumer adopts these schemas.

See [extraction validation](docs/validation.md) for the local checks and remaining
clean-build/runtime validation.
