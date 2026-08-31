add_executable(bench_bus_latency bench_bus_latency.cpp)
target_link_libraries(bench_bus_latency PRIVATE rdb rdb_flags)

add_executable(bench_queue_throughput bench_queue_throughput.cpp)
target_link_libraries(bench_queue_throughput PRIVATE rdb rdb_flags)
