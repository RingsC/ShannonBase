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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Column Unit.
*/
#ifndef __SHANNONBASE_CU_H__
#define __SHANNONBASE_CU_H__

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"  //uintxxx

#include "storage/innobase/include/ut0dbg.h"

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/imcs/table_meta.h"
#include "storage/rapid_engine/imcs/var_len_data.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/memory_pool.h"
class Field;
namespace ShannonBase {
class ShannonBaseContext;
class Rapid_load_context;

namespace Imcs {
class Dictionary;
class Chunk;
class RapidTable;
class RpdTable;
class Cu : public MemoryObject {
 public:
  using cu_fd_t = uint64;
  using Cu_header = struct SHANNON_ALIGNAS Cu_header_t {
   public:
    // a copy of source field info, only use its meta info. do NOT use it
    // directly.
    Field *m_source_fld{nullptr};

    // width size of this cu.
    size_t m_width{0};

    // field type of this cu.
    enum_field_types m_type{MYSQL_TYPE_NULL};

    // encoding type. pls ref to:
    // https://dev.mysql.com/doc/heatwave/en/mys-hw-varlen-encoding.html
    // https://dev.mysql.com/doc/heatwave/en/mys-hw-dictionary-encoding.html
    Compress::Encoding_type m_encoding_type{Compress::Encoding_type::NONE};

    // local dictionary.
    std::unique_ptr<Compress::Dictionary> m_local_dict;

    // charset info of this  CU.
    const CHARSET_INFO *m_charset;

    // statistics info.
    std::atomic<double> m_max{SHANNON_MIN_DOUBLE}, m_min{SHANNON_MAX_DOUBLE}, m_middle{0}, m_median{0}, m_avg{0},
        m_sum{0};

    // belongs to which table.
    RapidTable *m_owner;

    // key length of this cu.
    std::atomic<size_t> m_key_len{0};
  };

  Cu(RapidTable *owner, const Field *field);
  Cu(RapidTable *owner, const Field *field, std::string name);

  virtual ~Cu();

  Cu(Cu &&) = delete;
  Cu &operator=(Cu &&) = delete;

  /** write the data to a cu. `data` the data will be written. returns the
  address where the data wrote. the data append to tail of Cu. returns nullptr
  failed.*/
  uchar *write_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len);

  /** read the data from this Cu, traverse all chunks in cu to get the data from
  where m_r_ptr locates. */
  uchar *read_row(const Rapid_load_context *context, uchar *data, size_t len);

  // delete the row by rowid.
  uchar *delete_row(const Rapid_load_context *context, row_id_t rowid);

  // delete all the data from this cu.
  uchar *delete_row_all(const Rapid_load_context *context);

  // update the data located at rowid with new value-'data'.
  uchar *update_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len);

  // update the data located at rowid with new value-'data'.
  uchar *update_row_from_log(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len);

  // get the chunk header.
  inline Cu_header *header() { return m_header.get(); }

  // gets the base address of CU.
  inline uchar *base() {
    std::shared_lock lk(m_mutex);
    if (!m_chunks.size()) return nullptr;
    return m_chunks[0].get()->base();
  }

  // get the pointer of chunk with its id.
  inline Chunk *chunk(uint id) {
    std::shared_lock lk(m_mutex);
    if (id >= m_chunks.size()) return nullptr;
    return m_chunks[id].get();
  }

  // get how many chunks in this CU.
  inline uint32 chunks() const { return m_chunks.size(); }

  // returns the normalized length, the text type encoded with uint32.
  inline size_t normalized_pack_length() {
    std::shared_lock lk(m_mutex);
    if (m_chunks.empty()) return 0;
    return m_chunks[0]->normalized_pack_length();
  }

  // return the real pack length: field->pack_length.
  inline size_t pack_length() {
    std::shared_lock lk(m_mutex);
    if (m_chunks.empty()) return 0;
    return m_chunks[0]->pack_length();
  }

  inline size_t field_length() {
    std::shared_lock lk(m_mutex);
    if (m_chunks.empty()) return 0;
    return m_chunks[0]->field_length();
  }

  inline size_t field_length_bytes() {
    std::shared_lock lk(m_mutex);
    if (m_chunks.empty()) return 0;
    return m_chunks[0]->field_length_bytes();
  }

  inline std::string &keystr() { return m_cu_key; }

  inline std::shared_mutex &get_mutex() { return m_mutex; }

  void get_current_statistics(bool force_recalc = false) {
    if (force_recalc || m_stats_dirty.load(std::memory_order_relaxed)) {
      recalculate_statistics();
    }
  }

 private:
  // init chunks header.
  void init_header(Cu_header *header, RapidTable *owner, const Field *field);

  // inti chunk body, mainly the memory space.
  void init_body(const Field *field);

  // get the field value. if field is string/text then return its stringid.
  // or, do nothing.
  uchar *get_vfield_value(uchar *&data, size_t &len, bool need_pack = false);

  // alloclate a new chunk if it's needed.
  void ensure_new_chunks(size_t required_chunk_id);

  // upda the header info, such as row count, sum, avg, etc.
  // if row_reserved = true, means the prows has been added.
  void update_meta_info(OPER_TYPE type, uchar *data, uchar *old, bool row_reserved = false);

  // re-calculate the statistics.
  void recalculate_statistics();

  template <typename T, typename Compare>
  void update_atomic_value(std::atomic<T> &atomic_var, T new_val, Compare comp) {
    T current_val = atomic_var.load(std::memory_order_relaxed);
    while (comp(current_val, new_val)) {
      if (atomic_var.compare_exchange_weak(current_val, new_val, std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
        break;  // update success
      }
      // current_val is already the latest.
    }
  }

  void update_min_atomic(double new_val) {
    update_atomic_value(m_header->m_min, new_val, [](double current, double candidate) { return current > candidate; });
  }

  void update_max_atomic(double new_val) {
    update_atomic_value(m_header->m_max, new_val, [](double current, double candidate) { return current < candidate; });
  }

 private:
  // mutex of cu
  std::shared_mutex m_mutex;

  // header info of this Cu.
  std::unique_ptr<Cu_header> m_header{nullptr};

  // chunks in this cu.
  std::vector<std::unique_ptr<Chunk>> m_chunks;

  // reader pointer.
  std::atomic<uint32> m_current_chunk{0};

  // key name of this cu.
  std::string m_cu_key;

  typedef struct alignas(CACHE_LINE_SIZE) LocalDataBuffer_t {
    uchar data[MAX_FIELD_WIDTH];
  } LocalDataBuffer;
  // TODO: it corrupt under multithread, will fixed in future.
  // static SHANNON_THREAD_LOCAL LocalDataBuffer m_buff;

  // the meta stat mutex.
  std::mutex m_stats_mutex;
  //// the statistics is refreshed.
  std::atomic<bool> m_stats_dirty{false};

  // magic number for CU.
  const char *m_magic = "SHANNON_CU";
};

class Imcu;
class ColumnStatistics {
 public:
  struct BasicStats {
    // Numerical statistics
    double min_value;
    double max_value;
    double sum;
    double avg;
    double variance;  // Variance
    double stddev;    // Standard deviation

    // Count statistics
    uint64_t row_count;       // Total row count
    uint64_t null_count;      // NULL count
    uint64_t distinct_count;  // Unique value count (estimated)

    // Data characteristics
    double null_fraction;  // NULL fraction
    double cardinality;    // Cardinality (distinct_count / row_count)

    BasicStats();
  };

  struct StringStats {
    std::string min_string;  // Lexicographical minimum
    std::string max_string;  // Lexicographical maximum

    double avg_length;    // Average length
    uint64_t max_length;  // Maximum length
    uint64_t min_length;  // Minimum length

    uint64_t empty_count;  // Empty string count

    StringStats();
  };

  // Histogram
  /**
   * Equi-Height Histogram
   * - Each bucket contains the same number of rows
   * - Suitable for uneven data distribution
   */
  class EquiHeightHistogram {
   public:
    struct Bucket {
      double lower_bound;       // Lower bound
      double upper_bound;       // Upper bound
      uint64_t count;           // Row count
      uint64_t distinct_count;  // Distinct value count

      Bucket();
    };

    explicit EquiHeightHistogram(size_t num_buckets = 64);

    /**
     * Build histogram
     */
    void build(const std::vector<double> &values);

    /**
     * Estimate selectivity
     */
    double estimate_selectivity(double lower, double upper) const;

    /**
     * Estimate equality selectivity
     */
    double estimate_equality_selectivity(double value) const;

    /**
     * Get total row count
     */
    uint64_t get_total_rows() const;

    /**
     * Get bucket count
     */
    size_t get_bucket_count() const;

    /**
     * Get buckets
     */
    const std::vector<Bucket> &get_buckets() const;

   private:
    std::vector<Bucket> m_buckets;
    size_t m_bucket_count;
  };

  // Quantiles
  struct Quantiles {
    static constexpr size_t NUM_QUANTILES = 100;  // Percentiles

    double values[NUM_QUANTILES + 1];  // 0%, 1%, 2%, ..., 100%

    Quantiles();

    /**
     * Compute quantiles
     */
    void compute(const std::vector<double> &sorted_values);

    /**
     * Get specified percentile
     */
    double get_percentile(double p) const;

    /**
     * Get median
     */
    double get_median() const;
  };

  // HyperLogLog (Cardinality Estimation)
  /**
   * HyperLogLog algorithm
   * - Used to estimate number of distinct values (NDV)
   * - Space complexity: O(m), where m is number of registers
   * - Error rate: approximately 1.04 / sqrt(m)
   */
  class HyperLogLog {
   public:
    HyperLogLog();

    /**
     * Add value
     */
    void add(uint64_t hash);

    /**
     * Estimate cardinality
     */
    uint64_t estimate() const;

    /**
     * Merge another HyperLogLog
     */
    void merge(const HyperLogLog &other);

   private:
    static constexpr size_t NUM_REGISTERS = 1024;  // 2^10
    static constexpr size_t REGISTER_BITS = 10;

    std::vector<uint8_t> m_registers;

    /**
     * Count leading zeros
     */
    static uint8_t count_leading_zeros(uint64_t x);
  };

  // Sampler
  /**
   * Reservoir Sampling
   * - Used for random sampling of fixed number of samples
   */
  class Reservoir_Sampler {
   public:
    explicit Reservoir_Sampler(size_t sample_size = 10000);

    /**
     * Add value
     */
    void add(double value);

    /**
     * Get samples
     */
    const std::vector<double> &get_samples() const;

    /**
     * Get sample rate
     */
    double get_sample_rate() const;

   private:
    std::vector<double> m_samples;
    size_t m_sample_size;
    size_t m_seen_count;
  };

  ColumnStatistics(uint32_t col_id, const std::string &col_name, enum_field_types col_type);

  /**
   * Update statistics (single value)
   */
  void update(double value);

  /**
   * Update statistics (string)
   */
  void update(const std::string &value);

  /**
   * Record NULL value
   */
  void update_null();

  /**
   * Finalize statistics (compute derived values)
   */
  void finalize();

  const BasicStats &get_basic_stats() const;

  const StringStats *get_string_stats() const;

  const EquiHeightHistogram *get_histogram() const;

  const Quantiles *get_quantiles() const;

  /**
   * Estimate selectivity (range query)
   */
  double estimate_range_selectivity(double lower, double upper) const;

  /**
   * Estimate selectivity (equality query)
   */
  double estimate_equality_selectivity(double value) const;

  /**
   * Estimate NULL selectivity
   */
  double estimate_null_selectivity() const;

  bool serialize(std::ostream &out) const;

  bool deserialize(std::istream &in);

  void dump(std::ostream &out) const;

 private:
  // Column metadata
  uint32_t m_column_id;
  std::string m_column_name;
  enum_field_types m_column_type;

  // Basic statistics
  BasicStats m_basic_stats;

  // String statistics (optional)
  std::unique_ptr<StringStats> m_string_stats;

  // Histogram
  std::unique_ptr<EquiHeightHistogram> m_histogram;

  // Quantiles
  std::unique_ptr<Quantiles> m_quantiles;

  // HyperLogLog (cardinality estimation)
  std::unique_ptr<HyperLogLog> m_hll;

  // Sampler
  std::unique_ptr<Reservoir_Sampler> m_sampler;

  // Update time
  std::chrono::system_clock::time_point m_last_update;

  // Version number (for detecting staleness)
  uint64_t m_version;

  /**
   * Compute variance
   */
  void compute_variance();

  /**
   * Build histogram
   */
  void build_histogram();

  /**
   * Compute quantiles
   */
  void compute_quantiles();

  /**
   * Check if string type
   */
  bool is_string_type() const;
};

class CU : public MemoryObject {
 public:
  // CU Header
  struct CU_header {
    // Basic Information
    Imcu *owner_imcu{nullptr};           // Back reference to IMCU
    std::atomic<uint32_t> column_id{0};  // Column index

    Field *field_metadata{nullptr};            // Field metadata
    enum_field_types type{MYSQL_TYPE_NULL};    // Data type
    std::atomic<size_t> pack_length{0};        // Original length
    std::atomic<size_t> normalized_length{0};  // Normalized length
    const CHARSET_INFO *charset{nullptr};

    // Encoding and Compression
    Compress::Encoding_type encoding{Compress::Encoding_type::NONE};
    Compress::Compression_level compression_level{Compress::Compression_level::DEFAULT};

    // Local dictionary (for string encoding)
    std::shared_ptr<Compress::Dictionary> local_dict{nullptr};

    // Column-Level Statistics (within IMCU)
    std::atomic<double> min_value{DBL_MAX};
    std::atomic<double> max_value{DBL_MIN};
    std::atomic<double> sum{0};
    std::atomic<double> avg{0};

    std::atomic<size_t> total_count{0};
    std::atomic<size_t> null_count{0};
    std::atomic<size_t> distinct_count{0};  // Estimated value

    // Data Layout
    std::atomic<size_t> capacity{0};         // Capacity (number of rows)
    std::atomic<size_t> data_size{0};        // Actual data size
    std::atomic<size_t> compressed_size{0};  // Compressed size

    // Version Information
    std::atomic<size_t> version_count{0};      // Number of versions
    std::atomic<size_t> version_data_size{0};  // Version data size
  };

 public:
  CU(Imcu *owner, const FieldMetadata &field_meta, uint32_t col_idx, size_t capacity,
     std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool);

  virtual ~CU();

  /**
   * Write value (for INSERT)
   * @param local_row_id: Local row ID
   * @param data: Data pointer (NULL handled by IMCU's null_mask)
   * @param len: Data length
   * @return: Returns true if successful
   */
  bool write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len);

  /**
   * Update value (for UPDATE)
   * - Create column-level version
   * - Write new value
   * @param local_row_id: Local row ID
   * @param new_data: New data
   * @param len: Length
   * @param context: Context (contains transaction information)
   * @return: Returns SHANNON_SUCCESS if successful
   */
  int update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len);

  /**
   * Batch write (optimized version, for initial loading)
   * @param start_row: Starting row
   * @param data_array: Data array
   * @param count: Number of rows
   */
  bool write_batch(const Rapid_context *context, row_id_t start_row, const std::vector<uchar *> &data_array,
                   size_t count);

  /**
   * Read value (returns current value, does not consider versions)
   * @param local_row_id: Local row ID
   * @param buffer: Output buffer
   * @return: Data length, returns UNIV_SQL_NULL for NULL
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const;

  /**
   * Read value (specified SCN version)
   * @param local_row_id: Local row ID
   * @param target_scn: Target SCN
   * @param buffer: Output buffer
   * @return: Data length
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uint64_t target_scn, uchar *buffer) const;

  /**
   * Get value pointer (zero-copy, for scanning)
   * @param local_row_id: Local row ID
   * @return: Data pointer (directly points to internal buffer)
   */
  const uchar *read(const Rapid_context *context, row_id_t local_row_id);

  /**
   * Batch read (vectorized)
   * @param row_ids: List of row IDs to read
   * @param output: Output buffer (pre-allocated)
   * @return: Number of rows read
   */
  size_t read_batch(const Rapid_context *context, const std::vector<row_id_t> &row_ids, uchar *output) const;

  /**
   * Scan column (continuous read)
   * @param start_row: Starting row
   * @param count: Number of rows
   * @param output: Output buffer
   */
  size_t scan_range(const Rapid_context *context, row_id_t start_row, size_t count, uchar *output) const;

  /**
   * Create version (called during UPDATE)
   * @param local_row_id: Local row ID
   * @param context: Context
   */
  void create_version(const Rapid_context *context, row_id_t local_row_id);

  /**
   * Get version count
   */
  inline size_t get_version_count(const Rapid_context *context) const { return m_version_manager->get_version_count(); }

  /**
   * Clean up old versions
   * @param min_active_scn: Minimum active SCN
   * @return: Number of bytes reclaimed
   */
  size_t purge_versions(const Rapid_context *context, uint64_t min_active_scn);

  /**
   * Dictionary encoding (for strings)
   * @param data: Original data
   * @param len: Length
   * @param encoded: Encoded value (dictionary ID)
   * @return: Returns true if successful
   */
  bool encode_value(const Rapid_context *context, const uchar *data, size_t len, uint32_t &encoded);

  /**
   * Dictionary decoding
   * @param encoded: Dictionary ID
   * @param buffer: Output buffer
   * @param len: Output length
   * @return: Returns true if successful
   */
  bool decode_value(const Rapid_context *context, uint32_t encoded, uchar *buffer, size_t &len) const;

  /**
   * Compress column data (background compression)
   */
  bool compress();

  /**
   * Decompress column data
   */
  bool decompress();

  /**
   * Update statistics
   */
  void update_statistics(const uchar *data, size_t len);

  /**
   * Get column statistics
   */
  ColumnStatistics get_statistics() const;

  inline size_t get_data_size() const { return m_header.data_size; }
  inline size_t get_capacity() const { return m_header.capacity; }
  inline enum_field_types get_type() const { return m_header.type; }
  inline size_t get_normalized_length() const { return m_header.normalized_length; }
  inline Field *get_source_field() const { return m_header.field_metadata; }

  bool serialize(std::ostream &out) const;
  bool deserialize(std::istream &in);

  /**
   * Get data address
   */
  inline const uchar *get_data_address(row_id_t local_row_id) const {
    if (local_row_id >= m_header.capacity) return nullptr;
    return (m_data.get() + local_row_id * m_header.normalized_length);
  }

 private:
  /**
   * Check if dictionary encoding is needed
   */
  inline bool needs_dictionary() const { return true; }

  /**
   * Calculate numeric statistics
   */
  double get_numeric_value(const Rapid_context *context, const uchar *data, size_t len) const;

 private:
  const char *m_magic = "SHANNON_CU";

  CU_header m_header;

  // Data Storage
  // Main data area (continuous memory, normalized length)
  struct PoolDeleter {
    ShannonBase::Utils::MemoryPool *pool;
    size_t size;

    PoolDeleter(ShannonBase::Utils::MemoryPool *p, size_t s) : pool(p), size(s) {}
    PoolDeleter() : pool(nullptr), size(0) {}
    PoolDeleter(const PoolDeleter &other) = default;
    PoolDeleter &operator=(const PoolDeleter &other) = default;

    void operator()(uchar *ptr) const {
      if (pool && ptr) {
        pool->deallocate(ptr, size);
      }
    }
  };

  std::unique_ptr<uchar[], PoolDeleter> m_data;
  std::atomic<size_t> m_data_capacity;

  // Variable-length data area (optional, for long strings)
  std::unique_ptr<VarlenDataPool> m_varlen_pool;

  // Column-Level Version Management
  /**
   * Column Version Manager
   * - Only creates versions for modified rows
   * - Only saves old values for this column
   */
  class Column_Version_Manager {
   public:
    struct Column_Version {
      Transaction::ID txn_id;
      uint64_t scn;
      std::chrono::system_clock::time_point timestamp;

      // Column value (does not include metadata, metadata is in IMCU's Transaction Journal)
      std::unique_ptr<uchar[]> old_value;
      size_t value_length;

      Column_Version *prev{nullptr};  // Version chain

      Column_Version() : value_length(0), prev(nullptr) {}
    };

   private:
    // key: local_row_id, value: version chain head
    std::unordered_map<row_id_t, std::unique_ptr<Column_Version>> m_versions;
    mutable std::shared_mutex m_mutex;

   public:
    /**
     * Create version
     */
    void create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn, const uchar *old_value,
                        size_t len);

    /**
     * Get value at specified SCN
     */
    bool get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer, size_t &len) const;

    /**
     * Clean up old versions
     */
    size_t purge(uint64_t min_active_scn);

    /**
     * Get version count
     */
    size_t get_version_count() const {
      std::shared_lock lock(m_mutex);
      return m_versions.size();
    }
  };

  std::unique_ptr<Column_Version_Manager> m_version_manager;

  // Locking and Concurrency Control
  // CU-level lock (protects data writes)
  // Read operations are typically lock-free (consistency ensured by IMCU's visibility judgment)
  std::mutex m_data_mutex;

  // CU local Memory Management Pool.
  std::shared_ptr<ShannonBase::Utils::MemoryPool> m_memory_pool;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CU_H__