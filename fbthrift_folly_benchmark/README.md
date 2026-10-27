# fbthrift Echo 压测示例（C++）

基于 Facebook Thrift (fbthrift) 的 Echo RPC 压测工具，包含：

- Thrift IDL 定义（含多种数据类型）
- fbthrift 服务端（press_server）
- fbthrift 压测客户端（press_client）
- 支持 Rocket / Header 两种传输协议
- 支持 zlib / zstd / none 压缩
- 支持选择性填充数据类型（`--fields` gflag）

---

## 一、IDL 数据结构

```thrift
namespace cpp2 press_test

struct Request {
    1: bool   echo_attachment;
    2: binary payload;
    3: i32    int_val;
    4: i64    long_val;
    5: double double_val;
    6: string str_val;
    7: list<i32>  int_list;
    8: list<string> str_list;
    9: set<string> str_set;
    10: map<string, i64> kv_map;
}

struct PerfTestResponse {
    1: double cpu_usage;
    2: double mem_usage;
    3: binary payload;
}

service PerfTestService {
    PerfTestResponse test(1: Request req);
}
```

---

## 二、环境依赖

- C++17 或更高
- fbthrift / fizz / wangle / folly / glog / fmt
- libsodium / gflags / boost / lz4 / zstd / libevent / double-conversion / libunwind

依赖库安装到 `ins/` 目录下：

```
ins/
├── fbthrift/
├── fizz/
├── folly/
└── wangle/
```

---

## 三、代码生成与编译

```bash
# 生成 gen-cpp2
thrift1 --gen mstch_cpp2 press.thrift

# 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

---

## 四、服务端启动

| 参数 | 默认值 | 说明 |
|------|------|------|
| `--port` | 8002 | 服务端监听端口 |
| `--num_io_threads` | 1 | I/O 工作线程数 |
| `--num_cpu_threads` | 1 | CPU 工作线程数 |
| **`--use_io_uring`** | **false** | **io_uring 开关（IoUringBackend + setPreferIoUring）** |

```bash
./press_server --num_io_threads=12 --num_cpu_threads=12 --port=8002

# 开 io_uring
./press_server --use_io_uring=true --num_io_threads=12 --num_cpu_threads=12 --port=8002
```

---

## 五、客户端参数

| 参数 | 默认值 | 说明 |
|------|------|------|
| `--host` | 127.0.0.1 | 服务端 IP |
| `--port` | 8002 | 服务端端口 |
| `--transport` | rocket | 传输协议：rocket / header |
| `--payload_size` | -1 | Payload 大小（字节），<0 则自动扫 1~1024 |
| `--io_threads` | 10 | I/O 线程数（EventBase 数） |
| `--cpu_threads` | 0 | CPU 线程数（0=hardware_concurrency） |
| `--queue_depth` | 1 | 每连接最大 in-flight 请求数 |
| `--test_seconds` | 20 | 压测持续时间（秒） |
| `--test_iterations` | 0 | 按次数跑（0=按时间） |
| `--compression` | none | 压缩算法：none / zlib / zstd |
| `--rpc_timeout_ms` | 2000 | RPC 超时（毫秒） |
| `--connections` | 100 | 客户端连接总数 |
| `--echo_attachment` | false | 是否回显 payload |
| `--mode` | close_loop | 压测模式：close_loop / open_loop |
| `--expected_qps` | 0 | 限速 QPS（0=不限） |
| **`--fields`** | **all** | **数据类型选择** |
| `--random_fields` | false | 随机模式开关 |
| `--random_seed` | 0 | 随机种子（0=真随机，非零=可复现） |
| `--str_val_size` | 32 | 随机字符串长度（字节） |
| `--int_list_size` | 5 | 随机 int_list 元素数 |
| `--str_list_size` | 5 | 随机 str_list 元素数 |
| `--kv_map_size` | 5 | 随机 kv_map 键值对数 |
| `--verbose_rpc_error` | false | 打印 RPC 错误详情 |
| **`--use_io_uring`** | **false** | **客户端 io_uring 开关（IoUringBackend + AsyncIoUringSocket）** |

### `--fields` 用法

控制 Request 中哪些字段参与序列化，逗号分隔：

```bash
# 全部类型（默认）
./press_client ... --fields=all

# 只测 binary payload（纯 echo）
./press_client ... --fields=none

# 指定组合
./press_client ... --fields=int_val,str_val
./press_client ... --fields=int_list,str_list,str_set,kv_map
./press_client ... --fields=double_val,long_val,kv_map
```

可选值：`int_val`, `long_val`, `double_val`, `str_val`, `int_list`, `str_list`, `str_set`, `kv_map`, `all`, `none`

### `--random_fields` 用法

开启后每个字段生成随机内容，结合 `--random_seed` 控制可复现性：

```bash
# 随机 + 可复现
./press_client ... --fields=all --random_fields --random_seed=42

# 真随机
./press_client ... --fields=all --random_fields

# 随机 + 控制大小
./press_client ... --fields=all --random_fields --str_val_size=256 --int_list_size=20 --kv_map_size=10

# 纯 binary（关闭所有复杂字段）
./press_client ... --fields=none
```

---


### 随机值字节控制（CompactProtocol varint）

 开启后，整数字段按**目标字节数**随机生成，避免全范围随机导致的包大小波动：

| 字段 | 字节范围 | 编码方式 |
|------|------|------|
|  (i32) | 1~5 字节等概率 | zigzag + varint |
|  (i64) | 1~10 字节等概率 | zigzag + varint |
|  每个元素 | 1~5 字节等概率 | 同上 |
|  | 固定 8 字节 | IEEE 754，无变长 |
|  | 由  控制 | 前缀 varint(len) + 原始字节 |

> 目的： 和  的**实际网络包大小期望值相同**，Throughput 对比才有意义。

---

## 六、压测示例

```bash
./press_client \
 --host=127.0.0.1 \
 --port=8002 \
 --transport=rocket \
 --payload_size=1024 \
 --queue_depth=10 \
 --test_seconds=120 \
 --compression=none \
 --rpc_timeout_ms=0 \
 --io_threads=12 \
 --cpu_threads=12 \
 --connections=1000 \
 --echo_attachment=false \
 --fields=all
```

---

## 七、示例压测结果

```
====== Benchmark Results =======
Mode: close_loop
Total duration: 120.000s
Total requests: 66508185
Success: 66508185
Fail: 0
QPS: 554234.88
Throughput: 541.24MB/s
Latency (us): avg=540.71 min=161 max=1095512 p50=513 p90=649 p99=752 p999=8900
Server CPU (%): avg=1591.11 p50=1593.85 p99=1707.27
Client CPU (%): avg=2008.29 p50=2017.92 p99=2092.29
================================
```
