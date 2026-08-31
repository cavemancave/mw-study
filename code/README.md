# 配套代码：机器人数据总线实验室（rdb）

教程里的机制不能只停留在 Markdown。这个目录是可以 `clone` 下来直接编译、跑测试、跑压测的实现，
用来验证书中的每一条结论。

**定位**：它不是又一个中间件框架，而是一组**最小可运行、可测量、可失败**的教学实现。
所有取舍都服务于"看清内部发生了什么"，不追求功能完整。

## 环境要求

| 项 | 要求 |
| --- | --- |
| 编译器 | GCC 11+ 或 Clang 14+（C++20） |
| 构建 | CMake 3.16+ |
| 系统 | Linux（当前模块为可移植实现，后续加入的共享内存/epoll 模块将是 Linux 专属） |

## 构建与运行

```bash
# 学习与调试（开 -Werror，跑得慢但能抓住问题）
cmake -S code -B build -DCMAKE_BUILD_TYPE=Debug -DRDB_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 排查数据竞争
cmake -S code -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DRDB_SANITIZER=thread
cmake --build build-tsan --parallel && ctest --test-dir build-tsan --output-on-failure

# 排查内存错误与未定义行为
cmake -S code -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRDB_SANITIZER=address
cmake --build build-asan --parallel && ctest --test-dir build-asan --output-on-failure

# 性能测量：必须 -O2 且不带 sanitizer，否则数字没有参考价值
cmake -S code -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel --parallel
./build-rel/bench/bench_bus_latency --rate 1000 --count 200000 --subscribers 3 --payload 4096
./build-rel/bench/bench_queue_throughput --count 2000000 --capacity 1024 --producers 4
```

CMake 选项：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `RDB_BUILD_TESTS` | ON | 构建单元测试 |
| `RDB_BUILD_BENCH` | ON | 构建压测程序 |
| `RDB_WERROR` | OFF | 警告即错误 |
| `RDB_SANITIZER` | none | `address`（含 UBSan）/ `thread` / `undefined` |

> ASan 与 TSan 不能同时开启。ASan 通常让程序慢 2 倍以上，TSan 可达 5–15 倍。

## 代码与章节映射

| 头文件 | 对应章节 | 它证明了什么 |
| --- | --- | --- |
| `include/rdb/common/time.h` | 第 6、7 章 | 单调时钟 vs 墙上时钟的分工；定频节拍不累积漂移 |
| `include/rdb/concurrency/bounded_queue.h` | 第 2、4 章 | 有界队列的三种溢出策略、优雅关闭、高水位可观测 |
| `include/rdb/concurrency/spsc_ring.h` | 第 2、6 章 | acquire/release 内存序、缓存行填充、无锁的适用边界 |
| `include/rdb/concurrency/thread_pool.h` | 第 2、7 章 | 有界任务队列、任务被拒绝的返回路径、drain vs discard |
| `include/rdb/msg/header.h` | 第 3、8 章 | 定长小端消息头、schema 指纹、解码必须校验 |
| `include/rdb/msg/buffer_pool.h` | 第 3、6 章 | 大消息复用内存、共享句柄扇出、池耗尽即背压信号 |
| `include/rdb/transport/framing.h` | 第 2、4 章 | 字节流分帧、长度前缀的安全上限、流错位必须断链 |
| `include/rdb/bus/qos.h` | 第 4、5 章 | RxO 兼容规则：可靠性、持久性、deadline |
| `include/rdb/bus/topic_bus.h` | 第 4 章 | 发布订阅内核：订阅者隔离、latch、lifespan、不兼容不静默 |
| `include/rdb/metrics/histogram.h` | 第 6、10 章 | 用 p50/p90/p99/p999 而不是均值描述延迟 |

## 实验清单与验收标准

每条都是可执行的，验收标准是**可观察的数字**，而不是"跑通了"。

### 实验 1：溢出策略如何改变你丢掉的数据

```bash
ctest --test-dir build -R test_bounded_queue --output-on-failure
```

验收：`DropNewest` 保留最早两条，`DropOldest` 保留最新两条，`dropped()` 均为 1。
思考题：IMU 话题该选哪种？控制指令话题呢？

### 实验 2：慢订阅者到底拖垮了谁

```bash
ctest --test-dir build -R test_topic_bus --output-on-failure
./build-rel/bench/bench_bus_latency --rate 500 --count 5000 --subscribers 3 --slow-ms 5
```

验收：`sub[0]`（被人为拖慢）的 `dropped` 明显大于 0，而 `sub[1]`、`sub[2]` 的 `dropped` 为 0，
且 publish 侧的 `rate` 仍接近 500 msg/s。这就是"每订阅者一队列一线程"带来的隔离。

### 实验 3：可靠 QoS 的代价

```bash
./build-rel/bench/bench_bus_latency --rate 2000 --count 100000 --depth 8
./build-rel/bench/bench_bus_latency --rate 2000 --count 100000 --depth 8 --best-effort
```

验收：Reliable 版本 `failed` 可能非 0 且 p99 更高；BestEffort 版本 `dropped` 非 0 但 p99 更稳。
把两组 p50/p99/p999 和 dropped 记下来——面试里说"可靠更好"而给不出这张表，就是没做过。

### 实验 4：无锁不等于更快

```bash
./build-rel/bench/bench_queue_throughput --count 2000000 --capacity 1024 --producers 4
```

验收：记录三行吞吐。`SpscRing` 在单生产者单消费者下通常明显领先，
但把 `--capacity` 调到 4 再跑一次，观察忙等（yield）带来的 CPU 浪费。
结论应该是"看争用和批量"，不是"无锁更快"。

### 实验 5：分帧与解码的失败路径

```bash
ctest --test-dir build -R "test_framing|test_header" --output-on-failure
```

验收：逐字节喂入仍能正确重组 3 条帧；超长长度前缀和错误魔数都进入 `error()` 状态。
把 `max_frame_bytes` 去掉再想一遍：对端发一个 4GB 的长度前缀会发生什么？

### 实验 6：sanitizer 能抓到什么

```bash
cmake -S code -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DRDB_SANITIZER=thread
cmake --build build-tsan --parallel && ctest --test-dir build-tsan --output-on-failure
```

验收：全部通过。然后**故意**把 `bounded_queue.h` 里 `size()` 的加锁删掉再跑，
观察 TSan 报出的数据竞争栈。会用工具定位比记住结论更重要。

## 持续集成

`.github/workflows/ci.yml` 在每次改动 `code/` 时运行：

- GCC 与 Clang × Debug/RelWithDebInfo 四组构建 + `ctest`（Debug 开 `-Werror`）
- ASan+UBSan 与 TSan 两组构建 + `ctest`

本地无 Linux 环境时，以 CI 结果为准。

## 当前边界

诚实列出还没有做的部分，避免把教学实现误当成生产件：

- 仅进程内通信；跨进程共享内存、Unix 域套接字、epoll 事件循环尚未加入。
- `TopicBus` 每个订阅者一个线程，订阅者数量很大时应改为线程池 + 亲和分组。
- `BufferPool` 的 `shared_ptr` 自定义删除器每次获取会分配一个控制块；极端热路径需换成侵入式引用计数。
- 没有跨机传输、发现协议、录制格式与多机状态同步——这些在后续步骤中加入。
