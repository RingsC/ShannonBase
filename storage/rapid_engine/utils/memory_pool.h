/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_UTILS_MEMORY_POOL_H__
#define __SHANNONBASE_UTILS_MEMORY_POOL_H__

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "storage/rapid_engine/include/rapid_const.h"
namespace ShannonBase {
namespace Utils {
class MemoryPool {
 public:
  enum class SubPoolType {
    SMALL_BLOCK,  // Similar to 64KB POOL - metadata, small objects
    LARGE_BLOCK   // Similar to 1MB POOL - large data blocks
  };

  enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

  struct PoolStats {
    std::atomic<size_t> total_capacity{0};
    std::atomic<size_t> allocated_bytes{0};
    std::atomic<size_t> used_bytes{0};
    std::atomic<size_t> peak_usage{0};
    std::atomic<size_t> allocation_count{0};
    std::atomic<size_t> deallocation_count{0};
    std::atomic<size_t> expansion_count{0};
    std::atomic<size_t> defragmentation_count{0};
    std::atomic<size_t> failed_allocations{0};

    struct Snapshot {
      size_t total_capacity;
      size_t allocated_bytes;
      size_t used_bytes;
      size_t peak_usage;
      size_t allocation_count;
      size_t deallocation_count;
      size_t expansion_count;
      size_t defragmentation_count;
      size_t failed_allocations;
      double usage_percentage;
      double fragmentation_ratio;
    };

    [[nodiscard]] Snapshot snapshot() const noexcept;
  };

  struct TenantConfig {
    std::string name;
    size_t quota;
    std::atomic<size_t> current_usage{0};
    std::atomic<size_t> peak_usage{0};
    std::atomic<size_t> allocation_count{0};

    TenantConfig(const std::string &n, size_t q);
  };

  struct Config {
    size_t initial_size;
    double small_pool_ratio;
    size_t alignment;
    bool allow_expansion;
    size_t min_expansion_size;
    double expansion_trigger_threshold;
    bool auto_defragmentation;
    double defrag_trigger_threshold;
    LogLevel log_level;

    Config()
        : initial_size(SHANNON_DEFAULT_MEMRORY_SIZE),
          small_pool_ratio(0.2),
          alignment(CACHE_LINE_SIZE),
          allow_expansion(true),
          min_expansion_size(128 * SHANNON_MB),
          expansion_trigger_threshold(0.9),  // 90%
          auto_defragmentation(true),
          defrag_trigger_threshold(0.3),  // 30%
          log_level(LogLevel::INFO) {}
  };

  explicit MemoryPool(const Config &config = Config());
  virtual ~MemoryPool() noexcept;

  MemoryPool(const MemoryPool &) = delete;
  MemoryPool &operator=(const MemoryPool &) = delete;

  // Memory allocation/deallocation
  void *allocate(size_t size, SubPoolType pool_type = SubPoolType::LARGE_BLOCK, const std::string &tenant_id = "");
  void *allocate_auto(size_t size, const std::string &tenant_id = "");
  void deallocate(void *ptr, size_t size) noexcept;

  // Memory pool management
  bool expand(size_t additional_size);
  bool defragment();
  void reset() noexcept;

  // Tenant management
  void set_tenant_quota(const std::string &tenant_id, size_t quota);
  size_t get_tenant_usage(const std::string &tenant_id) const;

  // Statistics and configuration
  void print_stats() const noexcept;
  [[nodiscard]] PoolStats::Snapshot stats() const noexcept;
  void set_log_level(LogLevel level);

 private:
  struct FreeBlock {
    size_t offset;
    size_t size;
  };

  struct SubPool {
    void *memory_base;
    size_t total_size;
    size_t current_offset;
    std::vector<FreeBlock> free_blocks;
    mutable std::mutex mutex;

    SubPool(size_t size, size_t alignment);
    ~SubPool();
  };

  struct AllocationInfo {
    size_t offset;
    size_t aligned_size;
    int pool_index;
    std::string tenant_id;
  };

  Config m_config;
  std::vector<std::unique_ptr<SubPool>> m_subpools;
  std::unordered_map<void *, AllocationInfo> m_allocations;
  std::unordered_map<std::string, std::unique_ptr<TenantConfig>> m_tenants;

  mutable std::mutex m_alloc_mutex;
  mutable std::mutex m_tenant_mutex;
  PoolStats m_stats;

  std::thread m_monitor_thread;
  std::atomic<bool> m_shutdown;

  // Internal methods
  void validate_config();
  void initialize_pools(size_t total_size);
  void *allocate_from_pool(int pool_idx, size_t aligned_size, size_t actual_size, const std::string &tenant_id);
  bool expand_subpool(int pool_index, size_t additional_size);
  bool defragment_subpool(int pool_index);
  void merge_adjacent_free_blocks(SubPool *subpool);
  void *try_allocate_from_free_blocks(SubPool *subpool, size_t aligned_size);
  void record_allocation(void *ptr, size_t aligned_size, size_t actual_size, int pool_index,
                         const std::string &tenant_id);
  bool check_tenant_quota(const std::string &tenant_id, size_t size);
  void update_tenant_usage(const std::string &tenant_id, ssize_t delta);
  void update_peak_usage() noexcept;
  void monitor_loop();
  void cleanup() noexcept;
  void log(LogLevel level, const std::string &message) const;

  // Static utility methods
  static size_t align_up(size_t size, size_t alignment) noexcept;
  static void *aligned_alloc_portable(size_t alignment, size_t size) noexcept;
  static void free_aligned_portable(void *ptr) noexcept;
  static std::string format_size(size_t bytes);
};
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_MEMORY_POOL_H__