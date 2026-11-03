#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
uint64_t perf_now_us();
void perf_observe(const char* stage, uint64_t elapsed_us);
void perf_count(const char* name, uint64_t delta = 1);
void perf_encoder_queue(size_t depth, uint64_t oldest_us);
void perf_send_queue(int fd, size_t depth, size_t bytes, uint64_t oldest_us);
std::string perf_json();
void perf_start();
