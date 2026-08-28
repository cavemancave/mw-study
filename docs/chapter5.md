---
layout: default
title: 第 5 章：ROS 2、DDS 与框架选型
---

# 第 5 章：ROS 2、DDS 与框架选型

## 本章目标

以 ROS 2 + DDS 为主线理解机器人通信框架，同时知道 Zenoh、ZeroMQ、Cyber RT 和 DORA 各自解决什么问题，避免把框架 API 当成中间件原理。

## 5.1 ROS 2 的组成

ROS 2 提供节点、topic、service、action、参数、生命周期和工具链；底层通过 RMW 抽象连接 DDS 等实现。DDS 通常提供类型支持、发现、发布订阅、QoS 和传输。

一条 ROS 2 消息可能经历：类型生成 -> 节点发现 -> endpoint 匹配 -> 序列化/loan -> 传输 -> 接收缓存 -> executor 调度 -> 回调处理。性能问题可能发生在任一阶段。

## 5.2 DDS 需要掌握什么

理解 participant、publisher/subscriber、data writer/reader、discovery、reliability、durability、history、deadline 和 liveliness。不要假设不同 RMW 的默认值、发现方式和共享内存行为一致；部署时必须固定实现、版本和配置并做实测。

## 5.3 Executor 与组件

单线程 executor 容易被慢回调阻塞；多线程 executor 需要 callback group、锁和对象生命周期设计。高优先级控制回调应和大图像处理隔离，回调内避免阻塞 I/O。组件化和 intra-process 能减少边界成本，但跨进程仍可能发生序列化或复制。

## 5.4 框架对比

- **ROS 2/DDS**：机器人生态完整，类型、发现、QoS 和工具成熟。
- **Zenoh**：适合分层路由、边云协同和资源表达，但要学习其路由和存储语义。
- **ZeroMQ**：轻量消息模式灵活，发现、schema、QoS 和治理多由应用负责。
- **Cyber RT**：偏向自动驾驶数据流、调度和共享内存，需要理解其 channel、record 和 component 模型。
- **DORA**：强调数据流和节点执行，适合作为计算图/数据流框架对比对象。

选型维度应包含语义完整度、生态、发现、实时性、跨语言、共享内存、弱网、运维和团队经验，而非只看吞吐。

## 5.5 真实案例：ROS 2 不自动等于零拷贝

同进程组件可能使用 intra-process 优化，跨进程则依赖具体 RMW 和共享内存配置。大消息在不同边界可能仍复制。必须用实际消息类型、QoS、进程布局和 RMW 版本验证，而不能从文档中的“支持零拷贝”推断端到端无复制。

## 5.6 实验与验收

用 ROS 2 创建 IMU、图像和控制 topic，再加入 service/action。更换至少一种 RMW，比较不同 QoS、executor 线程数、进程布局和消息大小下的 p50/p99、CPU、RSS 和丢帧。

验收标准：能画出消息路径；能解释 QoS 不匹配和 executor 阻塞；能说明框架的能力、默认行为和仍需自研的部分。

## 面试问题与参考答案

**问：DDS 和 ZeroMQ 的根本区别是什么？**

答：DDS 是面向数据分发的完整通信模型，包含类型、发现和 QoS；ZeroMQ 是灵活的消息传输库，提供模式但不替应用定义完整的数据契约和治理。选择取决于系统语义和生态要求。

**问：如何降低 ROS 2 延迟？**

答：先分段测量，再检查 executor、回调阻塞、QoS、序列化、进程边界、共享内存、队列深度和 CPU 调度；优化后同时比较 p99、CPU、内存和丢帧，不能只看平均值。
