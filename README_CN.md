## 远程对象存储的本地只读视图

该仓库提供一个可复用的 Go 包（`pkg/remotefs`）、一个命令行工具（`cmd/remotefs-cli`）以及一个常驻守护进程（`cmd/remotefs-daemon`），让应用能够把对象存储当成固定前缀下的本地目录来访问。它只暴露只读的 `stat`、`ls`、`cat`/`open` 能力，因此适合无法挂载 FUSE、无法加载内核模块或磁盘空间紧张的环境。

### 设计概览

- **ObjectStore 抽象**：`pkg/objectstore` 定义了 RemoteFS 需要的最小接口（`Head`、`List`、`Download`）。仓库包含基于 AWS SDK v2 的 S3 实现（`NewS3Store`），也可以实现新的 object store 并注入。
- **受限磁盘缓存**：`pkg/cache` 暴露一个有硬性容量上限的磁盘 LRU 缓存。文件内容首次读取时写入缓存，后续直接命中，超出配额会自动淘汰。
- **RemoteFS 接口**：`pkg/remotefs` 负责把传入的本地路径转换为对象存储 key，公开 `ReadFile`、`ReadDir`、`Stat` 等接口。它在 daemon 启动时会调用 `WarmMetadataCache` 遍历远端目录，缓存元数据后，后续单文件 `stat` 可以直接命中本地缓存，减少 RPC。
- **CLI 集成测试工具**：`cmd/remotefs-cli` 提供 `stat`、`ls`、`cat` 命令以及 `serve` 子命令，方便在没有 daemon 的情况下验证 IPC API。
- **IPC 守护进程**：`cmd/remotefs-daemon` 通过 Unix Socket 或本地 TCP 暴露 `/stat`、`/ls`、`/cat` HTTP 接口，始终返回固定的 UID/GID 和 `0550/0440` 权限，保证 `ls -la` 类工具展示一致。
- **LD_PRELOAD Shim**：`shim/ldpreload` 提供可注入的共享库，拦截 `open`/`stat` 等系统调用，并把请求代理到 daemon，实现对旧程序的透明兼容。

### 快速开始

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

上述命令会：

1. 把所有访问限制在 `/data/virtual`，保证不会越权访问其他路径。
2. 直接向 S3 拉取目录数据，无需挂载任何远程文件系统。
3. 只有文件内容会写入磁盘缓存；目录结构常驻内存，确保低延迟。

### CLI IPC 模式

```bash
go build ./cmd/remotefs-cli
./remotefs-cli \
  -bucket my-bucket \
  -prefix project/data \
  -region ap-southeast-1 \
  -local-root /data/virtual \
  serve -socket /tmp/remotefs.sock
```

`serve` 子命令与 daemon 对齐，同样暴露 `/stat`、`/ls`、`/cat`，并在缺失对象时返回 `"<path>: No such file or directory"`，方便 POSIX 客户端处理。

### 守护进程

```bash
go build ./cmd/remotefs-daemon
./remotefs-daemon \
  -bucket my-bucket \
  -prefix project/data \
  -region ap-southeast-1 \
  -local-root /data/virtual \
  -socket /tmp/remotefs.sock
```

启动后即可通过 Unix Socket 发送 HTTP 请求：

```bash
curl --unix-socket /tmp/remotefs.sock http://unix/stat?path=/data/virtual/readme.txt
curl --unix-socket /tmp/remotefs.sock http://unix/ls?path=/data/virtual
curl --unix-socket /tmp/remotefs.sock http://unix/cat?path=/data/virtual/bigfile > /tmp/bigfile
```

每个接口都校验 `local-root`，语义对齐 `stat(2)`、`readdir(3)` 与只读的 `open(2)+read(2)`。

### LD_PRELOAD 使用

```bash
make -C shim/ldpreload
export REMOTEFS_SOCKET=/tmp/remotefs.sock
export REMOTEFS_ROOT=/data/virtual
export REMOTEFS_SHIM_CACHE=/var/tmp/remotefs-cache
LD_PRELOAD=$PWD/shim/ldpreload/libremotefs_shim.so ls -la /data/virtual
```

该 shim 会自动 URL 编码、根据需要下载文件并缓存，确保多个目录和多级子目录都能正确列出。

### 注意事项

- 项目只实现读取逻辑（stat/ls/cat/open），未提供写操作。
- 守护进程启动后会主动预热元数据缓存；如果数据量巨大，可根据实际情况调小 `-timeout` 或改成离线批处理。
- 当前运行环境无法联网拉取 AWS SDK 依赖，请在可联网环境执行 `go mod tidy` 后再构建，以生成完整的 `go.sum`。
