## Remote Object Storage View without mount/fuse

The repository now contains a reusable Go package (`pkg/remotefs`), a CLI
(`cmd/remotefs-cli`), and a long-running daemon (`cmd/remotefs-daemon`) that let
an application interact with an object store as if it were reading files beneath
a regular local path prefix.
This satisfies the requirement of providing "local" filesystem semantics on top
of object storage in environments that cannot load kernel modules, mount remote
filesystems, or spare much disk capacity.

### Design overview

- **ObjectStore abstraction** – `pkg/objectstore` defines a small interface for
  operations the virtual filesystem needs. The repository ships with an S3
  implementation built on the AWS SDK v2 (`NewS3Store`).
- **Disk constrained cache** – `pkg/cache` exposes an on-disk LRU cache with a
  hard byte quota. Reads stream into this cache and reuse the existing data the
  next time the same file is requested. Evictions are automatic once the quota
  is exceeded, keeping storage usage bounded.
- **RemoteFS facade** – `pkg/remotefs` provides `ReadFile`, `ReadDir`, and
  `Stat` methods that mimic the `os` package while delegating to the configured
  `ObjectStore`. The caller passes the *local* path it wants to protect, and
  the filesystem ensures every operation stays within that prefix before
  translating it to the remote key. Only read-only operations are exposed to
  satisfy the requirement that the mounted path is view-only.
- **CLI for integration testing** – `cmd/remotefs-cli` implements simple
  commands (`ls`, `stat`, `cat`) and a `serve` mode that exposes the IPC API
  over a socket without running the full daemon. It proves that user
  applications can treat the configured local prefix as if it were a standard
  directory without touching mount or FUSE.
- **IPC daemon** – `cmd/remotefs-daemon` hosts an HTTP API over either a Unix
  domain socket or a loopback TCP port. It exposes `/stat`, `/ls`, and `/cat`
  endpoints that mirror common POSIX read calls so applications in any language
  can talk to the remote filesystem by issuing local IPC requests. The daemon
  (and the CLI `serve` mode) report metadata with the runtime UID/GID and fixed
  `0550`/`0440` permissions so tools like `ls -la` print consistent owners,
  groups, and modes even for deep directory trees.
- **LD_PRELOAD shim** – `shim/ldpreload/remotefs_shim.c` produces a shared
  library that can be injected into arbitrary Linux binaries. It intercepts
  `open`, `stat`, and directory traversal calls beneath the protected prefix
  and proxies them to the daemon, giving legacy programs a read-only view
  without code changes.

The components are dependency-free apart from the AWS SDK. If you target a
different object storage provider, implement the `ObjectStore` interface and
inject it into `remotefs.New`.

### Typical usage

```bash
go build ./cmd/remotefs-cli
./remotefs-cli \
  -bucket my-bucket \
  -prefix project/data \
  -region ap-southeast-1 \
  -local-root /data/virtual \
  -cache-dir /var/cache/remotefs \
  -cache-size $((256*1024*1024)) \
  ls /data/virtual/reports
```

The command above:

1. Locks the `local-root` prefix to `/data/virtual` so any other filesystem path
   is rejected immediately, satisfying the "only the specified path" rule.
2. Lists remote objects by delegating to S3 without mounting anything.
3. Stores only the directory metadata in memory. Actual object contents are
   downloaded lazily into the bounded cache when files are read. The cache
   keeps a tight byte budget to honor disk limits.

`pkg/remotefs` is intended to be imported directly by Go applications so that
their persistence layer can operate on *local-looking* paths while everything is
stored remotely. Applications written in other languages can call the CLI and
pipe data through stdin/stdout, talk to the daemon over IPC sockets, or preload
the shim library to get POSIX-like responses without re-implementing the Go
package.

### CLI IPC mode

```bash
go build ./cmd/remotefs-cli
./remotefs-cli \
  -bucket my-bucket \
  -prefix project/data \
  -region ap-southeast-1 \
  -local-root /data/virtual \
  serve -socket /tmp/remotefs.sock
```

The `serve` subcommand shares the daemon's `/stat`, `/ls`, and `/cat` endpoints
and always emits `404` responses with `"<path>: No such file or directory"` for
missing entries so POSIX clients see the same errors as the native Go package.

### Daemon usage

```bash
go build ./cmd/remotefs-daemon
./remotefs-daemon \
  -bucket my-bucket \
  -prefix project/data \
  -region ap-southeast-1 \
  -local-root /data/virtual \
  -socket /tmp/remotefs.sock
```

After the daemon starts you can issue HTTP requests through the Unix socket
using any language:

```bash
curl --unix-socket /tmp/remotefs.sock http://unix/stat?path=/data/virtual/readme.txt
curl --unix-socket /tmp/remotefs.sock http://unix/ls?path=/data/virtual
curl --unix-socket /tmp/remotefs.sock http://unix/cat?path=/data/virtual/bigfile > /tmp/bigfile
```

Each endpoint stays within the configured `local-root` path and mirrors the
behavior of `stat(2)`, `readdir(3)`, and read-only `open(2)+read(2)` calls.

### LD_PRELOAD shim

The `shim/ldpreload` directory contains a shared library that can be injected
into legacy binaries via `LD_PRELOAD`. It intercepts `stat`, `open`, and the
common directory traversal functions, proxies them to the daemon over the Unix
socket HTTP API, and returns metadata that always reports the current UID/GID
with directory mode `0550` and file mode `0440`. Missing paths are surfaced as
`"<path>: No such file or directory"` with `errno=ENOENT`, so utilities like
`ls -la` print the same ownership, permission, and error strings you would see
on a real filesystem even for deep directory trees.

Build the shim with:

```bash
make -C shim/ldpreload
```

Then inject it into any process after starting the daemon:

```bash
export REMOTEFS_SOCKET=/tmp/remotefs.sock
export REMOTEFS_ROOT=/data/virtual
export REMOTEFS_SHIM_CACHE=/var/tmp/remotefs-cache
LD_PRELOAD=$PWD/shim/ldpreload/libremotefs_shim.so ls -la /data/virtual
```

The shim automatically URL-encodes requests, downloads file contents into a
private cache directory when `open` is called, and supports multiple siblings
and nested descendants per the requirements.

### Notes & limitations

- The cache only stores file contents. Directory listings come straight from
  the object store, guaranteeing a consistent view.
- Only read paths are implemented. Extending the system with writes would
  require reintroducing upload/delete functionality, which is intentionally
  omitted here for safety.
- Because the execution environment blocked outbound network access, the Go
  module downloads for the AWS SDK could not be completed. Run `go mod tidy` in
  an environment with network connectivity before building to populate
  `go.sum`.
