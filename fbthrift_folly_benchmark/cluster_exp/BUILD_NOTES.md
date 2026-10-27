# suzhou950 构建记录

- 源码：folly @2aa28128d、fbthrift @68ba6854cc、fizz @f4d134caee、wangle @efafc25326（均为 tag v2022.11.14.00）。
- 安装前缀：`~/cluster_aware/ins`；系统依赖来自 openEuler 24.03 SP3 rpm（boost 1.83、fmt 8.1.1、glog 0.6、openssl 3.0 等）。
- 构建：`cluster_exp/build.sh <target>`；同步：本地 `cluster_exp/sync.sh`。

## 修补记录

- folly 在 `CMAKE_LIBRARY_ARCHITECTURE` 为空时假定 x86_64，给 aarch64 加上 `-msse4.2/-mavx` 导致编译失败 → `build.sh` 统一传 `-DCMAKE_LIBRARY_ARCHITECTURE=$(uname -m)-linux-gnu`（构建参数修复，未改源码）。
