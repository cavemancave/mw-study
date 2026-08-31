# 剩余任务与交接说明

> 这份文件是给"在开发容器里重新 clone 后继续做"的自己或 AI 看的。
> 请先读 `AGENTS.md`（用户长期要求）和 `docs/index.md`（教程入口），再读本文件。
> 完成一项就更新本文件的状态标记，不要一次做多项。

## 一、当前进度

用户的要求是：**已识别的 6 项改进全部要做，但分步骤，一次只做一项，做完再做下一项。**

| # | 改进项 | 状态 |
| --- | --- | --- |
| 1 | `code/` 可运行实验仓（CMake + 头文件库 + ctest + bench + CI + README 映射） | ✅ 已完成并验证 |
| 2 | 第 11 章落地：项目目录、模块接口、拼装代码、一键验收脚本 | ⬜ 未开始 |
| 3 | 站内导航：章节交叉链接、上一章/下一章、独立术语表页 | ⬜ 未开始 |
| 4 | JD 内容缺口：服务治理、意图交换、模型推理结果/训练数据闭环 | ⬜ 未开始 |
| 5 | 新增"如何测试中间件本身"章节 | ⬜ 未开始 |
| 6 | 次要盲点：实时调优、安全、跨语言绑定、大消息分片重组 | ⬜ 未开始 |

建议顺序：**2 → 3 → 4 → 5 → 6**（2 依赖第 1 项的代码；3 成本最低见效最快）。

## 二、第 1 项已经做了什么（已验证通过）

新增文件（全部未提交，见第五节）：

```
.github/workflows/ci.yml          GCC/Clang × Debug/RelWithDebInfo + ASan/TSan 六组 CI
.gitignore                        忽略 build/ build-*/ 等
code/CMakeLists.txt               C++20，选项 RDB_BUILD_TESTS/BENCH/WERROR/SANITIZER
code/README.md                    代码↔章节映射表 + 6 个实验及其可观察验收标准
code/include/rdb/
  testing/check.h                 零依赖微型测试框架（RDB_TEST / RDB_CHECK* / wait_for）
  common/time.h                   now_ns / wall_ns / Ticker（定频不累积漂移）
  concurrency/bounded_queue.h     三种溢出策略 + 优雅关闭 + 高水位统计
  concurrency/spsc_ring.h         无锁 SPSC 环，acquire/release + 缓存行填充
  concurrency/thread_pool.h       有界任务队列 + 任务拒绝路径 + drain/discard
  msg/header.h                    48 字节定长小端消息头 + FNV-1a schema 指纹
  msg/buffer_pool.h               定长缓冲池 + 共享句柄扇出 + 耗尽计数
  transport/framing.h             长度前缀分帧 + 最大帧上限 + 错位检测
  bus/qos.h                       RxO 兼容规则（可靠性/持久性/deadline）
  bus/topic_bus.h                 进程内 pub-sub 内核：订阅者隔离/latch/lifespan/不兼容不静默
  metrics/histogram.h             对数分桶直方图，O(1) 记录，p50/p90/p99/p999
code/tests/                       9 个测试文件，共 50 个用例
code/bench/                       bench_bus_latency、bench_queue_throughput
```

同时修改了：

- `docs/index.md`：新增"配套代码：机器人数据总线实验室"一节（模块↔章节表 + 构建命令 + 实验说明）。
- `docs/_config.yml`：`aux_links` 增加"配套代码"链接。

### 验证结果（2026-08-31，Windows + MinGW-w64 GCC 16.1.0，手工 g++ 编译）

- 9 个测试二进制、50 个用例**全部通过**。
- `-Wall -Wextra -Wpedantic -Wshadow` 下**零警告**。
- `bench_bus_latency --rate 2000 --count 20000 --subscribers 3 --payload 4096` 正常结束，无崩溃。
- `bench_queue_throughput --count 1000000 --capacity 1024 --producers 4` 正常结束。

已修复的 bug：`buffer_pool.h` 的 `acquire()` 一度捕获裸 `this` 作为删除器，导致句柄比池活得久时读到已释放内存（Windows 上表现为 `0xfeeefeee`）。现已改回 `weak_from_this()`，`buffer_pool_handle_outlives_pool` 用例覆盖该路径。

## 三、在开发容器里第一件要做的事

容器是 Linux，工具链齐全，**优先用 CMake + ctest 完整跑一遍**（Windows 上因无管理员权限装不了 CMake，只做了手工 g++ 验证）：

```bash
# 1) 正确性 + 警告即错误
cmake -S code -B build -DCMAKE_BUILD_TYPE=Debug -DRDB_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 2) 数据竞争（必须做，topic_bus/thread_pool 是多线程重灾区）
cmake -S code -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DRDB_SANITIZER=thread -DRDB_BUILD_BENCH=OFF
cmake --build build-tsan --parallel && ctest --test-dir build-tsan --output-on-failure

# 3) 内存错误与未定义行为
cmake -S code -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRDB_SANITIZER=address -DRDB_BUILD_BENCH=OFF
cmake --build build-asan --parallel && ctest --test-dir build-asan --output-on-failure

# 4) 性能基线（必须 -O2 且无 sanitizer）
cmake -S code -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel --parallel
./build-rel/bench/bench_bus_latency --rate 1000 --count 200000 --subscribers 3 --payload 4096
./build-rel/bench/bench_queue_throughput --count 2000000 --capacity 1024 --producers 4
```

Clang 也要过一遍：`-DCMAKE_CXX_COMPILER=clang++`。CI 里这六组配置都有。

## 四、已知问题与待办细节

### 4.1 `Ticker` 的定频精度（Linux 上需要复测）

`bench_bus_latency --rate 2000` 在 Windows 上实测只跑出 251 msg/s：`std::this_thread::sleep_until`
受 OS 定时器粒度限制（Windows 默认约 15.6ms）。Linux 上应显著改善，但 1kHz 以上仍可能不准。

待办：

- 在 Linux 上实测 100/1000/5000 Hz 的实际达成速率，把结论写进 `code/README.md`。
- `Ticker::sleep_until_next()` 返回的"是否丢拍"布尔值目前被 bench 忽略，应统计并打印 `missed_ticks`。
- 高频档位考虑改为"sleep 到接近目标再自旋"的混合策略，并在注释里说明它烧 CPU 的代价。

### 4.2 `_probe/` 是调试残留，不要提交

根目录的 `_probe/` 是排查上面那个 use-after-free 时的临时目录，含 `DrMemory-Windows-2.6.0/`
（几百 MB 的第三方工具）、`bisect.cpp`、`*.txt` 日志等。**重新 clone 后不会有这个目录，无需处理。**
如果在当前机器上提交，务必先删除或加入 `.gitignore`。

同理 `build/`、`build-chk/`、`build-rel/` 是 Windows 上的构建产物，已被 `.gitignore` 覆盖。

### 4.3 `code/README.md` 里的实验数据是占位的

第 1 项写了 6 个实验和验收标准，但**没有填真实测得的数字**。在容器里跑完后，
把实验 3（Reliable vs BestEffort 的 p50/p99/p999 与丢弃数）和实验 4（三种队列吞吐）
的真实数据填进 `code/README.md`，并在 `docs/chapter6.md` 引用同一组数字，保证书里书外一致。

### 4.4 CI 尚未在真实仓库上跑过

`.github/workflows/ci.yml` 已写好但从未触发。第一次 push 后检查六个 job 是否都绿；
Debug 组开了 `-Werror`，GCC 版本与本地不同可能报出新警告。

## 五、Git 状态

**所有第 1 项的产出都还没有提交**（用户要求：不要自动创建 commit）。当前工作区：

```
 M docs/_config.yml
 M docs/index.md
?? .github/
?? .gitignore
?? code/
?? _probe/          <- 调试残留，不要提交
```

建议的提交拆分（由用户决定何时执行）：

1. `add runnable code lab: bus, queues, buffer pool, framing, qos, histogram` —— `code/` + `.gitignore`
2. `add CI for code lab` —— `.github/`
3. `link tutorial to runnable code lab` —— `docs/index.md` + `docs/_config.yml`

## 六、后续各项的具体要求

### 第 2 项：第 11 章落地

`docs/chapter11.md` 目前 618 行，是最薄的一章，只有阶段描述和验收条目。需要补：

- 贯穿项目"机器人数据总线实验室"的完整目录结构与模块边界图。
- 各模块的 C++ 接口定义（头文件级），并说明与 `code/include/rdb/` 已有模块的对应关系。
- 主程序拼装代码：IMU/图像/点云/控制指令四条不同 QoS 的数据流跑起来。
- 一键验收脚本（跑压测 + 故障注入 + 生成性能报告），放到 `code/scripts/`。
- 该项应当**同时扩充 `code/`**：目前只有进程内通信，需要补 apps/ 和 scripts/。

### 第 3 项：站内导航

- 正文里所有"第 N 章"的纯文本引用改成 `chapterN.html` 链接（注意必须是 `.html`，见 `AGENTS.md`）。
- 每章末尾加"上一章 / 下一章"导航。
- 新建 `docs/glossary.md` 独立术语表页（front matter 需 `layout: default` / `title` / `parent` / `nav_order`），
  并在 `docs/index.md` 的术语约定一节链接过去。

### 第 4 项：JD 内容缺口

`JobDescription-amap.md` 里这几个关键词全书出现 0 次或近乎 0 次：

- **服务治理**：服务版本与灰度、配额下发、依赖治理、优雅上下线。建议放进第 4 章或新开小节。
- **意图交换**：第 9 章已有状态共享/任务协同/能力发现/拓扑管理，唯独缺意图交换。
- **模型推理结果 / 训练数据闭环**：大张量、GPU 内存、动态 shape、批处理对中间件的特殊要求。

### 第 5 项："如何测试中间件本身"

新开一章或并入第 10 章，内容：确定性测试与可注入时钟、反序列化 fuzz、
新旧版本兼容性矩阵、性能回归基线与门禁、并发用例如何写得不 flaky。
（`code/include/rdb/testing/check.h` 里的 `wait_for` 就是"不用固定 sleep 写并发测试"的例子，可直接引用。）

### 第 6 项：次要盲点

实时性调优（PREEMPT_RT / isolcpus / cgroup / `mlockall` / 线程优先级）、
安全（鉴权、传输加密、多租户隔离）、跨语言绑定（稳定 C ABI + Python）、
大消息跨机分片与重组。目前这些只有零散提及，需要专节。

## 七、写作与代码的硬约束（摘自 `AGENTS.md`，容易忘）

- 章节页必须保留 Jekyll front matter；站内链接用 `chapterN.html` 而不是 `.md`。
- 新增章节要同步更新 `docs/index.md` 的岗位映射表、学习路线图和导读。
- 每章保持 11 段固定结构，不要退化成提纲。
- 不要把"可靠""实时""零拷贝"当褒义标签，必须写清边界、代价和验证条件。
- 性能结论必须给 p50/p95/p99、吞吐、CPU、内存、带宽的具体数字和测量条件。
- **不要自动创建 git commit**，除非用户明确要求。
- 不要提交无关的格式化、依赖或个人配置文件。
