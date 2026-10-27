# suzhou950 构建记录

- 源码：folly @2aa28128d、fbthrift @68ba6854cc、fizz @f4d134caee、wangle @efafc25326（均为 tag v2022.11.14.00）。
- 安装前缀：`~/cluster_aware/ins`；系统依赖来自 openEuler 24.03 SP3 rpm（boost 1.83、fmt 8.1.1、glog 0.6、openssl 3.0 等）。
- 构建：`cluster_exp/build.sh <target>`；同步：本地 `cluster_exp/sync.sh`。

## 修补记录

- folly 在 `CMAKE_LIBRARY_ARCHITECTURE` 为空时假定 x86_64，给 aarch64 加上 `-msse4.2/-mavx` 导致编译失败 → `build.sh` 统一传 `-DCMAKE_LIBRARY_ARCHITECTURE=$(uname -m)-linux-gnu`（构建参数修复，未改源码）。
- 独立 CMake 工程链接已安装的 folly 时，`folly-config.cmake` 引用了 `gflags_shared` target，需先 `find_package(gflags CONFIG REQUIRED)` 再 `find_package(folly)`。
- **folly EventBase 修复**（folly-ca 分支单独提交，测试见 `folly/io/async/test/evb_wakeup/`）：libevent 2.1 的 `EVLOOP_ONCE` 在只处理了 internal 事件（通知队列、fbthrift reply queue）时不会返回，导致这些 handler 里排的 loop callback（rocket 写 flush、队列重新 arm）要等下一个外部事件才执行。未修复版本的安装保存在 `~/cluster_aware/ins-vanilla`（对照用）。
