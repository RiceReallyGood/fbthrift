# suzhou950 构建记录

- 源码：folly @2aa28128d、fbthrift @68ba6854cc、fizz @f4d134caee、wangle @efafc25326（均为 tag v2022.11.14.00）。
  - fbthrift 自带的依赖锁定（`build/deps/github_hashes/`）是 folly @30abf028d、wangle @efafc25326。tag 的 folly 比它多 2 个提交（ExceptionWrapper 警告修复、CacheLocality 编译提速），不涉及 executors / EventBase。
- 仓库：本地 folly、fbthrift 各有三个 remote，`folly-ca`、`fbthrift-ca` 是它们的 worktree。
  - `origin`：同事的 gitcode 仓库。
  - `upstream`：facebook 上游，只读，只拉需要的 tag。
  - `fork`：github.com/RiceReallyGood 的 fork，`cluster-aware-*` 分支和 tag 推到这里。
  - fork 是公开的，实验结果（`cluster_exp/results/`）只留在本地，已加入 `.gitignore`；带结果的原始历史在本地分支 `cluster-aware-bench-results`，不要推送。
- 安装前缀：`~/cluster_aware/ins`；系统依赖来自 openEuler 24.03 SP3 rpm（boost 1.83、fmt 8.1.1、glog 0.6、openssl 3.0 等）。
- 构建：`cluster_exp/build.sh <target>`。
- 同步：先把分支推到 fork，再在本地运行 `cluster_exp/sync.sh`。950 上的 `~/cluster_aware/src/{folly,fbthrift,fizz,wangle}` 是 git 仓库，脚本让它们从 GitHub 拉取并签出与本地 HEAD 相同的提交，版本记在 `~/cluster_aware/SOURCE_VERSIONS`。950 通过本机 HTTP 代理访问 GitHub（`git config --global http.https://github.com/.proxy http://127.0.0.1:10809`）。
- `src/folly-vanilla`（去掉 EventBase 修复的对照树）仍由 `sync_vanilla_folly.sh` 从 folly-ca HEAD 导出后 rsync。

## 修补记录

- folly 在 `CMAKE_LIBRARY_ARCHITECTURE` 为空时假定 x86_64，给 aarch64 加上 `-msse4.2/-mavx` 导致编译失败 → `build.sh` 统一传 `-DCMAKE_LIBRARY_ARCHITECTURE=$(uname -m)-linux-gnu`（构建参数修复，未改源码）。
- 独立 CMake 工程链接已安装的 folly 时，`folly-config.cmake` 引用了 `gflags_shared` target，需先 `find_package(gflags CONFIG REQUIRED)` 再 `find_package(folly)`。
- **folly EventBase 修复**（folly-ca 分支单独提交，测试见 `folly/io/async/test/evb_wakeup/`）：libevent 2.1 的 `EVLOOP_ONCE` 在只处理了 internal 事件（通知队列、fbthrift reply queue）时不会返回，导致这些 handler 里排的 loop callback（rocket 写 flush、队列重新 arm）要等下一个外部事件才执行。未修复版本的安装保存在 `~/cluster_aware/ins-vanilla`（对照用）。
