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

   The fundmental code for imcs.
*/
/**
 * The specification of IMCU, pls ref:
 * https://github.com/Shannon-Data/ShannonBase/issues/8
 * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCU1              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCU2              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 * ...
 *  * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCUN              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 *
 */
#ifndef __SHANNONBASE_IMCU_H__
#define __SHANNONBASE_IMCU_H__

#include <algorithm>
#include <atomic>  //std::atomic<T>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_list.h"    //for LIST
#include "sql/table.h"  //for TABLE

#include "storage/innobase/include/univ.i"  //UNIV_SQL_NULL
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/trx/transaction.h"  // Transaction_Journal
#include "storage/rapid_engine/utils/memory_pool.h"

class Field;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Imcu;
/**
 * Storage Index, Every IMCU header automatically creates and manages In-Memory Storage Indexes (IM storage indexes) for
 * its CUs. An IM storage index stores the minimum and maximum for all columns within the IMCU.
 * - Column statistics at IMCU level
 * - Used for query optimization: skip irrelevant IMCUs
 * - Similar to Oracle's Storage Index
 */
class StorageIndex {
 public:
  struct SHANNON_ALIGNAS ColumnStats {
    // Atomic basic statistics
    std::atomic<double> min_value;
    std::atomic<double> max_value;
    std::atomic<double> sum;
    std::atomic<double> avg;

    // Atomic NULL Statistics
    std::atomic<size_t> null_count;
    std::atomic<bool> has_null;

    // Atomic Cardinality Statistics
    std::atomic<size_t> distinct_count;  // Estimated value (HyperLogLog)

    // String Statistics - non-atomic, protected by mutex
    std::string min_string;  // Lexicographical minimum
    std::string max_string;  // Lexicographical maximum

    // Data Distribution - non-atomic, protected by mutex
    std::vector<double> histogram;  // Histogram

    // Mutex for string and vector operations
    mutable std::mutex m_string_mutex;

    ColumnStats()
        : min_value(DBL_MAX),
          max_value(DBL_MIN),
          sum(0.0),
          avg(0.0),
          null_count(0),
          has_null(false),
          distinct_count(0) {}
    ColumnStats(const ColumnStats &other)
        : min_value(other.min_value.load(std::memory_order_acquire)),
          max_value(other.max_value.load(std::memory_order_acquire)),
          sum(other.sum.load(std::memory_order_acquire)),
          avg(other.avg.load(std::memory_order_acquire)),
          null_count(other.null_count.load(std::memory_order_acquire)),
          has_null(other.has_null.load(std::memory_order_acquire)),
          distinct_count(other.distinct_count.load(std::memory_order_acquire)) {
      std::lock_guard lock(other.m_string_mutex);
      min_string = other.min_string;
      max_string = other.max_string;
      histogram = other.histogram;
    }

    ColumnStats &operator=(const ColumnStats &other) {
      if (this != &other) {
        min_value.store(other.min_value.load(std::memory_order_acquire));
        max_value.store(other.max_value.load(std::memory_order_acquire));
        sum.store(other.sum.load(std::memory_order_acquire));
        avg.store(other.avg.load(std::memory_order_acquire));
        null_count.store(other.null_count.load(std::memory_order_acquire));
        has_null.store(other.has_null.load(std::memory_order_acquire));
        distinct_count.store(other.distinct_count.load(std::memory_order_acquire));

        std::lock_guard lock1(m_string_mutex, std::adopt_lock);
        std::lock_guard lock2(other.m_string_mutex, std::adopt_lock);
        std::lock(m_string_mutex, other.m_string_mutex);

        min_string = other.min_string;
        max_string = other.max_string;
        histogram = other.histogram;
      }
      return *this;
    }

    ColumnStats(ColumnStats &&other) noexcept
        : min_value(other.min_value.load(std::memory_order_acquire)),
          max_value(other.max_value.load(std::memory_order_acquire)),
          sum(other.sum.load(std::memory_order_acquire)),
          avg(other.avg.load(std::memory_order_acquire)),
          null_count(other.null_count.load(std::memory_order_acquire)),
          has_null(other.has_null.load(std::memory_order_acquire)),
          distinct_count(other.distinct_count.load(std::memory_order_acquire)),
          min_string(std::move(other.min_string)),
          max_string(std::move(other.max_string)),
          histogram(std::move(other.histogram)) {}

    ColumnStats &operator=(ColumnStats &&other) noexcept {
      if (this != &other) {
        min_value.store(other.min_value.load(std::memory_order_acquire));
        max_value.store(other.max_value.load(std::memory_order_acquire));
        sum.store(other.sum.load(std::memory_order_acquire));
        avg.store(other.avg.load(std::memory_order_acquire));
        null_count.store(other.null_count.load(std::memory_order_acquire));
        has_null.store(other.has_null.load(std::memory_order_acquire));
        distinct_count.store(other.distinct_count.load(std::memory_order_acquire));

        std::lock_guard lock(m_string_mutex);
        min_string = std::move(other.min_string);
        max_string = std::move(other.max_string);
        histogram = std::move(other.histogram);
      }
      return *this;
    }
  };

 private:
  // Statistics for each column
  std::vector<ColumnStats> m_column_stats;

  // Dirty flag
  std::atomic<bool> m_dirty{false};

  // Concurrency control
  mutable std::shared_mutex m_mutex;

  // Number of columns
  size_t m_num_columns;

 public:
  StorageIndex(size_t num_columns) : m_num_columns(num_columns) {
    std::unique_lock lock(m_mutex);
    m_column_stats.resize(num_columns);
  }

  StorageIndex(const StorageIndex &other)
      : m_dirty(other.m_dirty.load(std::memory_order_acquire)), m_num_columns(other.m_num_columns) {
    std::shared_lock lock(other.m_mutex);
    m_column_stats = other.m_column_stats;
  }

  StorageIndex &operator=(const StorageIndex &other) {
    if (this != &other) {
      std::unique_lock lock1(m_mutex, std::defer_lock);
      std::shared_lock lock2(other.m_mutex, std::defer_lock);
      std::lock(lock1, lock2);

      m_column_stats = other.m_column_stats;
      m_num_columns = other.m_num_columns;
      m_dirty.store(other.m_dirty.load(std::memory_order_acquire));
    }
    return *this;
  }

  StorageIndex(StorageIndex &&other) noexcept
      : m_dirty(other.m_dirty.load(std::memory_order_acquire)), m_num_columns(other.m_num_columns) {
    std::unique_lock lock1(m_mutex, std::defer_lock);
    std::unique_lock lock2(other.m_mutex, std::defer_lock);
    std::lock(lock1, lock2);

    m_column_stats = std::move(other.m_column_stats);
    other.m_num_columns = 0;
  }

  StorageIndex &operator=(StorageIndex &&other) noexcept {
    if (this != &other) {
      std::unique_lock lock1(m_mutex, std::defer_lock);
      std::unique_lock lock2(other.m_mutex, std::defer_lock);
      std::lock(lock1, lock2);

      m_column_stats = std::move(other.m_column_stats);
      m_num_columns = other.m_num_columns;
      m_dirty.store(other.m_dirty.load(std::memory_order_acquire));
      other.m_num_columns = 0;
    }
    return *this;
  }

  /**
   * Update single column statistics (called during INSERT/UPDATE)
   * @param col_idx: Column index
   * @param value: Numeric value (converted)
   */
  void update(uint32_t col_idx, double value);

  /**
   * Update NULL statistics
   */
  void update_null(uint32_t col_idx);

  /**
   * Batch rebuild statistics (scan all data)
   * @param imcu: Owner IMCU
   */
  void rebuild(const Imcu *imcu);

  /**
   * Get column statistics - returns a snapshot
   */
  const ColumnStats *get_column_stats_snapshot(uint32_t col_idx) const;

  /**
   * Get individual atomic values (for read-only access)
   */
  inline double get_min_value(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return DBL_MAX;

    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].min_value.load(std::memory_order_acquire);
  }

  inline double get_max_value(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return DBL_MIN;

    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].max_value.load(std::memory_order_acquire);
  }

  inline size_t get_null_count(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return 0;

    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].null_count.load(std::memory_order_acquire);
  }

  inline bool get_has_null(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return false;

    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].has_null.load(std::memory_order_acquire);
  }

  /**
   * Check if IMCU can be skipped (based on predicates)
   * @param predicates: List of predicates
   * @return: Returns true if can be skipped
   */
  bool can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const;

  /**
   * Estimate selectivity
   * @param predicates: List of predicates
   * @return: Selectivity [0.0, 1.0]
   */
  double estimate_selectivity(const std::vector<Predicate> &predicates) const;

  /**
   * Update string statistics (requires mutex protection)
   */
  void update_string_stats(uint32_t col_idx, const std::string &value);

  inline void mark_dirty() { m_dirty.store(true, std::memory_order_relaxed); }

  inline bool is_dirty() const { return m_dirty.load(std::memory_order_acquire); }

  inline void clear_dirty() { m_dirty.store(false, std::memory_order_relaxed); }

  bool serialize(std::ostream &out) const;

  bool deserialize(std::istream &in);
};

/**
 * RowBuffer (Row Buffer)
 *
 * Purpose:
 * 1. Store one row of query result data
 * 2. Support zero-copy optimization (directly pointing to CU data)
 * 3. Handle type conversion and formatting
 *
 * Usage Scenarios:
 * - SELECT query result buffering
 * - UPDATE/INSERT input buffering
 * - Temporary buffering for row-level operations
 */
class RowBuffer {
 public:
  /**
   * Column Value Wrapper
   * - Supports both zero-copy and copy modes
   */
  struct SHANNON_ALIGNAS ColumnValue {
    // Data pointer (may point to CU internal data or owned buffer)
    const uchar *data;

    // Data length
    size_t length;

    // Flags
    struct SHANNON_ALIGNAS Flags {
      uint8_t is_null : 1;       // Whether NULL
      uint8_t is_zero_copy : 1;  // Whether zero-copy (directly points to CU)
      uint8_t is_encoded : 1;    // Whether encoded (dictionary ID)
      uint8_t needs_decode : 1;  // Whether needs decoding
      uint8_t reserved : 4;
    } flags;

    // Owned buffer (used when not zero-copy)
    std::unique_ptr<uchar[]> owned_buffer;

    // Column metadata
    enum_field_types type;

    ColumnValue();
    ColumnValue(const ColumnValue &other);
    ColumnValue(ColumnValue &&other) noexcept;

    ColumnValue &operator=(const ColumnValue &other);
    ColumnValue &operator=(ColumnValue &&other) noexcept;
  };

 private:
  // Row ID
  row_id_t m_row_id{0};

  // Column value array
  std::vector<ColumnValue> m_columns;

  // Number of columns
  size_t m_num_columns{0};

  // Whether all columns are zero-copy
  bool m_is_all_zero_copy{false};

  // Field metadata (optional, for type conversion)
  const std::vector<Field *> *m_field_metadata;

 public:
  /**
   * Constructor
   * @param num_columns: Number of columns
   * @param field_metadata: Field metadata (optional)
   */
  RowBuffer(size_t num_columns, const std::vector<Field *> *field_metadata = nullptr)
      : m_row_id(INVALID_ROW_ID),
        m_num_columns(num_columns),
        m_is_all_zero_copy(true),
        m_field_metadata(field_metadata) {
    m_columns.resize(num_columns);
  }

  RowBuffer(const RowBuffer &other)
      : m_row_id(other.m_row_id),
        m_num_columns(other.m_num_columns),
        m_is_all_zero_copy(other.m_is_all_zero_copy),
        m_field_metadata(other.m_field_metadata) {
    m_columns = other.m_columns;  // Utilize ColumnValue's copy constructor
  }

  RowBuffer(RowBuffer &&other) noexcept
      : m_row_id(other.m_row_id),
        m_columns(std::move(other.m_columns)),
        m_num_columns(other.m_num_columns),
        m_is_all_zero_copy(other.m_is_all_zero_copy),
        m_field_metadata(other.m_field_metadata) {
    other.m_row_id = ShannonBase::INVALID_ROW_ID;
    other.m_num_columns = 0;
  }

  RowBuffer &operator=(const RowBuffer &) = default;
  RowBuffer &operator=(RowBuffer &&) noexcept = default;

  virtual ~RowBuffer() = default;

  void set_row_id(row_id_t row_id) { m_row_id = row_id; }

  row_id_t get_row_id() const { return m_row_id; }

  // Column Value Setting (Zero-Copy)
  /**
   * Set column value (zero-copy mode)
   * - Directly points to CU data
   * - High performance, but requires ensuring CU lifecycle
   *
   * @param col_idx: Column index
   * @param data: Data pointer (points to CU internal)
   * @param length: Data length
   * @param type: Data type
   */
  void set_column_zero_copy(uint32_t col_idx, const uchar *data, size_t length,
                            enum_field_types type = MYSQL_TYPE_NULL);

  /**
   * Batch set column values (zero-copy)
   * @param data_ptrs: Array of data pointers
   * @param lengths: Array of lengths (optional)
   */
  void set_columns_zero_copy(const uchar **data_ptrs, const size_t *lengths = nullptr);

  // Column Value Setting (Copy)
  /**
   * Set column value (copy mode)
   * - Deep copy data to owned buffer
   * - Safe, but has performance overhead
   *
   * @param col_idx: Column index
   * @param data: Data pointer
   * @param length: Data length
   * @param type: Data type
   */
  void set_column_copy(uint32_t col_idx, const uchar *data, size_t length, enum_field_types type = MYSQL_TYPE_NULL);

  /**
   * Set NULL column
   */
  void set_column_null(uint32_t col_idx) {
    if (col_idx >= m_num_columns) return;

    ColumnValue &col = m_columns[col_idx];
    col.data = nullptr;
    col.length = UNIV_SQL_NULL;
    col.flags.is_null = 1;
    col.owned_buffer.reset();
  }

  // Column Value Reading
  /**
   * Get column value
   * @param col_idx: Column index
   * @return: Column value (read-only)
   */
  inline const ColumnValue *get_column(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return nullptr;
    return &m_columns[col_idx];
  }

  /**
   * Get column value (mutable)
   */
  inline ColumnValue *get_column_mutable(uint32_t col_idx) {
    if (col_idx >= m_num_columns) return nullptr;
    return &m_columns[col_idx];
  }

  /**
   * Check if column is NULL
   */
  inline bool is_column_null(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return true;
    return m_columns[col_idx].flags.is_null == 1;
  }

  /**
   * Get column data pointer
   */
  inline const uchar *get_column_data(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return nullptr;
    return m_columns[col_idx].data;
  }

  /**
   * Get column data length
   */
  inline size_t get_column_length(uint32_t col_idx) const {
    if (col_idx >= m_num_columns) return 0;
    return m_columns[col_idx].length;
  }

  // Type Conversion
  /**
   * Get column integer value
   */
  int64_t get_column_int(uint32_t col_idx) const;

  /**
   * Get column floating-point value
   */
  double get_column_double(uint32_t col_idx) const;

  /**
   * Get column string value
   * @param buffer: Output buffer
   * @param buffer_size: Buffer size
   * @return: Actual length
   */
  size_t get_column_string(uint32_t col_idx, char *buffer, size_t buffer_size) const;

  // Batch Operations
  /**
   * Clear all column values
   */
  void clear();
  void reset() { clear(); }

  /**
   * Convert to fully owned mode (for cross-IMCU transfer)
   */
  void convert_to_owned();

  // Serialization and Deserialization
  /**
   * Serialize to binary format
   * @param out: Output stream
   * @return: Returns true if successful
   */
  bool serialize(std::ostream &out) const;

  /**
   * Deserialize from binary format
   * @param in: Input stream
   * @return: Returns true if successful
   */
  bool deserialize(std::istream &in);

  // MySQL Compatibility Interface
  /**
   * Copy row data to MySQL Field array
   * @param fields: MySQL Field array
   * @param num_fields: Number of fields
   */
  bool copy_to_mysql_fields(Field **fields, size_t num_fields) const;

  /**
   * Read data from MySQL Field array
   * @param fields: MySQL Field array
   * @param num_fields: Number of fields
   */
  bool copy_from_mysql_fields(Field **fields, size_t num_fields);

  /**
   * Read data from MySQL row field data array
   * @param fields: MySQL Field array
   * @param[in] rowdata Row data buffer.
   * @param[in] len     Buffer length.
   * @param[in] col_offsets Column offsets.
   * @param[in] n_cols  Number of columns.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   */
  bool copy_from_mysql_fields(Field **fields, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                              ulong *null_byte_offsets, ulong *null_bitmasks);

  // Statistics and Debugging
  /**
   * Get number of columns
   */
  inline size_t get_num_columns() const { return m_num_columns; }

  /**
   * Whether all columns are zero-copy
   */
  inline bool is_all_zero_copy() const { return m_is_all_zero_copy; }

  /**
   * Get total data size (bytes)
   */
  inline size_t get_total_size() const {
    size_t total = 0;

    for (const auto &col : m_columns) {
      if (!col.flags.is_null) {
        total += col.length;
      }
    }

    return total;
  }

  /**
   * Get owned memory size (excluding zero-copy parts)
   */
  inline size_t get_owned_memory_size() const {
    size_t total = 0;

    for (const auto &col : m_columns) {
      if (col.owned_buffer) {
        total += col.length;
      }
    }

    return total;
  }

  /**
   * Print row content (for debugging)
   */
  void dump(std::ostream &out) const;

  /**
   * Validate row data integrity
   */
  bool validate() const;

  inline bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                            const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];

    // gets null byte.
    uchar null_byte = rowdata[byte_offset];

    // check null bit.
    return (null_byte & bitmask) != 0;
  }
};

// Auxiliary Type Definitions
/**
 * RowCallback (Row Callback Function Type)
 * Used for scan operation callbacks
 */
using RowCallback = std::function<void(row_id_t row_id, const std::vector<const uchar *> &row_data)>;

/**
 * RowBufferPool (Row Buffer Pool)
 * Used to reduce RowBuffer allocation overhead
 */
class RowBufferPool {
 private:
  std::vector<std::unique_ptr<RowBuffer>> m_buffers;
  std::mutex m_mutex;
  size_t m_num_columns;
  const std::vector<Field *> *m_field_metadata;

 public:
  RowBufferPool(size_t num_columns, const std::vector<Field *> *field_metadata = nullptr)
      : m_num_columns(num_columns), m_field_metadata(field_metadata) {}

  /**
   * Acquire a row buffer
   */
  std::unique_ptr<RowBuffer> acquire() {
    std::lock_guard lock(m_mutex);

    if (!m_buffers.empty()) {
      auto buffer = std::move(m_buffers.back());
      m_buffers.pop_back();
      buffer->reset();
      return buffer;
    }

    return std::make_unique<RowBuffer>(m_num_columns, m_field_metadata);
  }

  /**
   * Release row buffer
   */
  void release(std::unique_ptr<RowBuffer> buffer) {
    if (!buffer) return;

    std::lock_guard lock(m_mutex);

    // Limit pool size
    if (m_buffers.size() < 64) {
      buffer->clear();
      m_buffers.push_back(std::move(buffer));
    }
  }

  /**
   * Clear pool
   */
  void clear() {
    std::lock_guard lock(m_mutex);
    m_buffers.clear();
  }
};

/**
 * RowDirectory (Row Directory)
 *
 * Purpose:
 * 1. Quickly locate data positions of variable-length columns
 * 2. Support offset lookup after row-level compression
 * 3. Optimize column access for wide tables
 *
 * Usage Scenarios:
 * - Contains multiple variable-length columns (VARCHAR, TEXT, BLOB)
 * - Row-level compression is enabled
 * - Requires fast random access
 */
class RowDirectory {
 public:
  /**
   * Row Entry
   * - Records metadata for each row in the IMCU
   */
  struct SHANNON_ALIGNAS RowEntry {
    // Row start offset (relative to CU base address)
    uint32_t offset;

    // Actual row length (after compression or variable-length encoding)
    uint32_t length;

    // Row flags
    struct Flags {
      uint8_t is_compressed : 1;  // Whether compressed
      uint8_t is_deleted : 1;     // Whether deleted (redundant, for quick checking)
      uint8_t has_null : 1;       // Whether contains NULL
      uint8_t is_overflow : 1;    // Whether has overflow page
      uint8_t reserved : 4;       // Reserved bits
    } flags;

    // Checksum (optional, for data integrity checking)
    uint32_t checksum;

    RowEntry() : offset(0), length(0), checksum(0) { std::memset(&flags, 0, sizeof(flags)); }
  };

  /**
   * Column Offset Table (Optional)
   * - Used to quickly locate individual columns within a row
   * - Suitable for wide tables or scenarios with many variable-length columns
   */
  struct SHANNON_ALIGNAS ColumnOffsetTable {
    // Relative offset of each column within the row
    std::vector<uint16_t> column_offsets;

    // Actual length of each column
    std::vector<uint16_t> column_lengths;

    ColumnOffsetTable(size_t num_columns) {
      column_offsets.reserve(num_columns);
      column_lengths.reserve(num_columns);
    }
  };

 private:
  // Row entry array (fixed size, consistent with IMCU capacity)
  std::unique_ptr<RowEntry[]> m_entries;
  size_t m_capacity;

  // Column offset tables (optional, built on demand)
  // key: row_id, value: column offset table
  std::unordered_map<row_id_t, std::unique_ptr<ColumnOffsetTable>> m_column_offset_tables;

  // Whether column offset tables are enabled
  bool m_enable_column_offsets;

  // Number of columns (for initializing column offset tables)
  size_t m_num_columns;

  std::atomic<size_t> m_total_data_size{0};       // Total data size
  std::atomic<size_t> m_compressed_data_size{0};  // Compressed data size
  std::atomic<size_t> m_overflow_count{0};        // Overflow row count

  mutable std::shared_mutex m_mutex;

 public:
  /**
   * Constructor
   * @param capacity: IMCU capacity (number of rows)
   * @param num_columns: Number of columns
   * @param enable_column_offsets: Whether to enable column offset tables
   */
  RowDirectory(size_t capacity, size_t num_columns, bool enable_column_offsets = false)
      : m_capacity(capacity), m_enable_column_offsets(enable_column_offsets), m_num_columns(num_columns) {
    // Allocate row entry array
    m_entries = std::make_unique<RowEntry[]>(capacity);

    // Initialize all entries
    for (size_t i = 0; i < capacity; i++) {
      m_entries[i] = RowEntry();
    }
  }

  // Delete copy constructor and assignment operator
  RowDirectory(const RowDirectory &) = delete;
  RowDirectory &operator=(const RowDirectory &) = delete;

  // Allow move operations
  RowDirectory(RowDirectory &&other) noexcept;
  RowDirectory &operator=(RowDirectory &&other) noexcept;

  ~RowDirectory() = default;
  /**
   * Set row entry
   * @param row_id: Row ID
   * @param offset: Start offset
   * @param length: Data length
   * @param is_compressed: Whether compressed
   */
  void set_row_entry(row_id_t row_id, uint32_t offset, uint32_t length, bool is_compressed = false);

  /**
   * Get row entry
   * @param row_id: Row ID
   * @return: Row entry (read-only)
   */
  const RowEntry *get_row_entry(row_id_t row_id) const;

  /**
   * Mark row as deleted
   */
  void mark_deleted(row_id_t row_id);

  /**
   * Mark row as containing NULL
   */
  void mark_has_null(row_id_t row_id);

  /**
   * Mark row as having overflow page
   */
  void mark_overflow(row_id_t row_id);

  /**
   * Build column offset table (for wide tables with many variable-length columns)
   * @param row_id: Row ID
   * @param column_offsets: Column offset array
   * @param column_lengths: Column length array
   */
  void build_column_offset_table(row_id_t row_id, const std::vector<uint16_t> &column_offsets,
                                 const std::vector<uint16_t> &column_lengths);

  /**
   * Get column offset table
   * @param row_id: Row ID
   * @return: Column offset table pointer, returns nullptr if not exists
   */
  const ColumnOffsetTable *get_column_offset_table(row_id_t row_id) const;

  /**
   * Get column offset within row (fast path)
   * @param row_id: Row ID
   * @param col_idx: Column index
   * @return: Column offset, returns UINT16_MAX on failure
   */
  uint16_t get_column_offset(row_id_t row_id, uint32_t col_idx) const;

  /**
   * Get actual column length
   * @param row_id: Row ID
   * @param col_idx: Column index
   * @return: Column length, returns 0 on failure
   */
  uint16_t get_column_length(row_id_t row_id, uint32_t col_idx) const;

  /**
   * Batch get row offsets (for vectorized scanning)
   * @param start_row: Start row
   * @param count: Number of rows
   * @param offsets: Output offset array (pre-allocated)
   * @param lengths: Output length array (pre-allocated)
   */
  void get_batch_offsets(row_id_t start_row, size_t count, uint32_t *offsets, uint32_t *lengths) const;

  /**
   * Update compression statistics
   * @param row_id: Row ID
   * @param original_size: Original size
   * @param compressed_size: Compressed size
   */
  void update_compression_stats(row_id_t row_id, size_t original_size, size_t compressed_size);

  /**
   * Get compression ratio
   * @return: Compression ratio [0.0, 1.0]
   */
  double get_compression_ratio() const;

  /**
   * Get directory size (bytes)
   */
  size_t get_directory_size() const;

  /**
   * Get total data size
   */
  size_t get_total_data_size() const { return m_total_data_size.load(); }

  /**
   * Get compressed data size
   */
  size_t get_compressed_data_size() const { return m_compressed_data_size.load(); }

  /**
   * Get overflow row count
   */
  size_t get_overflow_count() const { return m_overflow_count.load(); }

  /**
   * Validate directory integrity
   * @return: true indicates integrity is normal
   */
  bool validate() const;

  /**
   * Print directory summary
   */
  void dump_summary(std::ostream &out) const;

  std::unique_ptr<RowDirectory> clone() const;

 private:
  /**
   * Calculate checksum (simple CRC32)
   */
  uint32_t compute_checksum(uint32_t offset, uint32_t length) const {
    // Simplified implementation, should use standard CRC32 in practice
    return offset ^ length ^ 0xDEADBEEF;
  }
};

class Imcu : public MemoryObject {
 public:
  // IMCU Header
  struct SHANNON_ALIGNAS Imcu_header {
    // Basic Information
    uint32_t imcu_id{0};                  // IMCU identifier
    row_id_t start_row{0};                // Global start row ID
    row_id_t end_row{0};                  // Global end row ID (exclusive)
    size_t capacity{0};                   // Capacity (number of rows)
    std::atomic<size_t> current_rows{0};  // Current number of rows

    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_modified;

    // Row-level Metadata (Shared). [TODO] In future we will use Hybrid Approach
    // Delete bitmap (shared across all columns)
    std::unique_ptr<bit_array_t> del_mask;

    // NULL bitmaps (per column)
    // null_masks[col_idx] = NULL bitmap of the column
    std::vector<std::unique_ptr<bit_array_t>> null_masks;

    // Lightweight transaction journal (metadata only)
    std::unique_ptr<TransactionJournal> txn_journal;

    // Storage index (per-column statistics)
    std::unique_ptr<StorageIndex> storage_index;

    // Optional row directory (for quick lookup of variable-length rows)
    std::unique_ptr<RowDirectory> row_directory;

    // IMCU-level Statistics
    std::atomic<uint64_t> insert_count{0};
    std::atomic<uint64_t> update_count{0};
    std::atomic<uint64_t> delete_count{0};

    std::atomic<double> avg_row_size{0};
    std::atomic<size_t> compressed_size{0};
    std::atomic<size_t> uncompressed_size{0};

    // Transaction Boundaries
    Transaction::ID min_trx_id{Transaction::MAX_ID};
    Transaction::ID max_trx_id{0};
    uint64_t min_scn{UINT64_MAX};
    uint64_t max_scn{0};

    // GC Information
    std::chrono::system_clock::time_point last_gc_time;
    size_t version_count{0};
    double delete_ratio{0.0};

    // Status Flags
    enum Status {
      ACTIVE,      // Writable
      READ_ONLY,   // Read-only (full)
      COMPACTING,  // Being compacted
      TOMBSTONE    // Marked for deletion
    };
    std::atomic<Status> status{ACTIVE};

    Imcu_header() = default;

    // Copy constructor (deep copy)
    Imcu_header(const Imcu_header &other)
        : imcu_id(other.imcu_id),
          start_row(other.start_row),
          end_row(other.end_row),
          capacity(other.capacity),
          current_rows(other.current_rows.load()),
          created_at(other.created_at),
          last_modified(other.last_modified),
          insert_count(other.insert_count.load()),
          update_count(other.update_count.load()),
          delete_count(other.delete_count.load()),
          avg_row_size(other.avg_row_size.load()),
          compressed_size(other.compressed_size.load()),
          uncompressed_size(other.uncompressed_size.load()),
          min_trx_id(other.min_trx_id),
          max_trx_id(other.max_trx_id),
          min_scn(other.min_scn),
          max_scn(other.max_scn),
          last_gc_time(other.last_gc_time),
          version_count(other.version_count),
          delete_ratio(other.delete_ratio),
          status(other.status.load()) {
      if (other.del_mask) del_mask = std::make_unique<bit_array_t>(*other.del_mask);
      null_masks.reserve(other.null_masks.size());
      for (const auto &mask : other.null_masks) {
        null_masks.emplace_back(mask ? std::make_unique<bit_array_t>(*mask) : nullptr);
      }
      if (other.txn_journal) txn_journal = std::make_unique<TransactionJournal>(std::move(*other.txn_journal));
      if (other.storage_index) storage_index = std::make_unique<StorageIndex>(*other.storage_index);
      if (other.row_directory) row_directory = other.row_directory->clone();
    }

    // Move constructor
    Imcu_header(Imcu_header &&other) noexcept
        : imcu_id(other.imcu_id),
          start_row(other.start_row),
          end_row(other.end_row),
          capacity(other.capacity),
          current_rows(other.current_rows.load()),
          created_at(other.created_at),
          last_modified(other.last_modified),
          del_mask(std::move(other.del_mask)),
          null_masks(std::move(other.null_masks)),
          txn_journal(std::move(other.txn_journal)),
          storage_index(std::move(other.storage_index)),
          row_directory(std::move(other.row_directory)),
          insert_count(other.insert_count.load()),
          update_count(other.update_count.load()),
          delete_count(other.delete_count.load()),
          avg_row_size(other.avg_row_size.load()),
          compressed_size(other.compressed_size.load()),
          uncompressed_size(other.uncompressed_size.load()),
          min_trx_id(other.min_trx_id),
          max_trx_id(other.max_trx_id),
          min_scn(other.min_scn),
          max_scn(other.max_scn),
          last_gc_time(other.last_gc_time),
          version_count(other.version_count),
          delete_ratio(other.delete_ratio),
          status(other.status.load()) {}

    Imcu_header &operator=(Imcu_header other) noexcept {
      swap(*this, other);
      return *this;
    }

    Imcu_header &operator=(Imcu_header &&other) noexcept {
      if (this != &other) {
        imcu_id = other.imcu_id;
        start_row = other.start_row;
        end_row = other.end_row;
        capacity = other.capacity;
        current_rows.store(other.current_rows.load());
        created_at = other.created_at;
        last_modified = other.last_modified;
        del_mask = std::move(other.del_mask);
        null_masks = std::move(other.null_masks);
        txn_journal = std::move(other.txn_journal);
        storage_index = std::move(other.storage_index);
        row_directory = std::move(other.row_directory);
        insert_count.store(other.insert_count.load());
        update_count.store(other.update_count.load());
        delete_count.store(other.delete_count.load());
        avg_row_size.store(other.avg_row_size.load());
        compressed_size.store(other.compressed_size.load());
        uncompressed_size.store(other.uncompressed_size.load());
        min_trx_id = other.min_trx_id;
        max_trx_id = other.max_trx_id;
        min_scn = other.min_scn;
        max_scn = other.max_scn;
        last_gc_time = other.last_gc_time;
        version_count = other.version_count;
        delete_ratio = other.delete_ratio;
        status.store(other.status.load());
      }
      return *this;
    }

    // Swap helper (for copy-and-swap)
    friend void swap(Imcu_header &a, Imcu_header &b) noexcept {
      using std::swap;
      swap(a.imcu_id, b.imcu_id);
      swap(a.start_row, b.start_row);
      swap(a.end_row, b.end_row);
      swap(a.capacity, b.capacity);
      a.current_rows.store(b.current_rows.load());
      swap(a.created_at, b.created_at);
      swap(a.last_modified, b.last_modified);
      swap(a.del_mask, b.del_mask);
      swap(a.null_masks, b.null_masks);
      swap(a.txn_journal, b.txn_journal);
      swap(a.storage_index, b.storage_index);
      swap(a.row_directory, b.row_directory);
      a.insert_count.store(b.insert_count.load());
      a.update_count.store(b.update_count.load());
      a.delete_count.store(b.delete_count.load());
      a.avg_row_size.store(b.avg_row_size.load());
      a.compressed_size.store(b.compressed_size.load());
      a.uncompressed_size.store(b.uncompressed_size.load());
      swap(a.min_trx_id, b.min_trx_id);
      swap(a.max_trx_id, b.max_trx_id);
      swap(a.min_scn, b.min_scn);
      swap(a.max_scn, b.max_scn);
      swap(a.last_gc_time, b.last_gc_time);
      swap(a.version_count, b.version_count);
      swap(a.delete_ratio, b.delete_ratio);
      a.status.store(b.status.load());
    }
  };

 public:
  Imcu(RpdTable *owner, Table_Metadata &table_meta, row_id_t start_row, size_t capacity,
       std::shared_ptr<Utils::MemoryPool> mem_pool);
  Imcu() = default;
  virtual ~Imcu();

  inline RpdTable *owner() { return m_owner_table; }

  inline Imcu_header::Status get_status() const { return m_header.status.load(std::memory_order_acquire); }

  inline void set_status(Imcu_header::Status status) { m_header.status.store(status, std::memory_order_release); }

  /**
   * Insert a row (IMCU-level entry point)
   * @param row_data: array of column data
   * @param context: execution context
   * @return local row_id within the IMCU, or INVALID_ROW_ID on failure
   */
  row_id_t insert_row(const Rapid_load_context *context, const RowBuffer &row_data);

  /**
   * Delete a row (core: mark deletion, column-independent)
   * @param local_row_id: local row ID within the IMCU
   * @param context: execution context
   * @return SHANNON_SUCCESS on sucess.
   */
  int delete_row(const Rapid_load_context *context, row_id_t local_row_id);

  /**
   * Batch delete (vectorized)
   * @param local_row_ids: list of local row IDs
   * @param context: execution context
   * @return number of deleted rows
   */
  size_t delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &local_row_ids);

  /**
   * Update a row (only modifies affected columns)
   * @param local_row_id: local row ID
   * @param updates: column index -> new value map
   * @param context: execution context
   * @return SHANNON_SUCCESS on success
   */
  int update_row(const Rapid_load_context *context, row_id_t local_row_id,
                 const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates);

  /**
   * Scan the IMCU (vectorized)
   * @param context: scan context
   * @param predicates: filter conditions
   * @param projection: list of projected columns
   * @param callback: row callback function
   * @return number of scanned rows
   */
  size_t scan(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
              const std::vector<uint32_t> &projection, RowCallback callback);

  /**
   * Check row visibility (core: single-row check)
   * @param context: read scan context
   * @param local_row_id: local row ID
   * @param reader_txn_id: reader transaction ID
   * @param reader_scn: reader SCN
   * @return true if visible
   */
  bool is_row_visible(Rapid_scan_context *context, row_id_t local_row_id, Transaction::ID reader_txn_id,
                      uint64_t reader_scn) const;

  /**
   * Batch visibility check (vectorized)
   * @param start_row: start row ID
   * @param count: number of rows
   * @param context: scan context
   * @param visibility_mask: output list of visible local row IDs
   */
  void check_visibility_batch(Rapid_scan_context *context, row_id_t start_row, size_t count,
                              bit_array_t &visibility_mask) const;

  /**
   * Read row data (specified columns)
   * @param local_row_id: local row ID
   * @param col_indices: indices of columns to read
   * @param context: scan context
   * @param output: output buffer
   */
  bool read_row(Rapid_scan_context *context, row_id_t local_row_id, const std::vector<uint32_t> &col_indices,
                RowBuffer &output);

  // Storage Index Operations
  /**
   * Determine if this IMCU can be skipped (based on Storage Index)
   * @param predicates: filter conditions
   * @return true if the IMCU can be skipped
   */
  bool can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const;

  /**
   * Update the Storage Index
   */
  void update_storage_index();

  // Maintenance
  /**
   * Garbage Collection
   * @param min_active_scn: minimum active SCN
   * @return number of bytes reclaimed
   */
  size_t garbage_collect(uint64_t min_active_scn);

  /**
   * Compact this IMCU (remove deleted rows)
   * @return new compacted IMCU instance
   */
  Imcu *compact();

  /**
   * Check if compaction is required
   */
  bool needs_compaction() const {
    return m_header.delete_ratio >= SHANNON_HIGH_DELETE_RATIO ||  // Deletion ratio exceeds 30%
           (m_header.delete_count.load() > SHANNON_LARGE_DELETE_COUNT &&
            m_header.delete_ratio >= SHANNON_MEDIUM_DELETE_RATIO);  // Or over 10,000 rows deleted with ratio > 20%
  }

  // Status Queries
  inline bool is_full() const {
    return m_header.current_rows.load(std::memory_order_acquire) >= m_header.capacity ||
           m_header.status.load(std::memory_order_acquire) == Imcu_header::READ_ONLY;
  }

  inline bool is_empty() const { return m_header.current_rows.load(std::memory_order_acquire) == 0; }

  inline bool is_null(uint32_t col_idx, row_id_t local_row_id) {
    if (col_idx > m_header.null_masks.size()) return false;
    return Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), local_row_id);
  }

  inline double get_usage_ratio() const {
    size_t current = m_header.current_rows.load(std::memory_order_acquire);
    if (m_header.capacity == 0) return 0.0;
    return static_cast<double>(current) / m_header.capacity;
  }

  size_t estimate_size() const {
    size_t total_size = 0;

    // Header size.
    total_size += sizeof(Imcu_header);

    // delete bit mask.Storage_Index
    if (m_header.del_mask) {
      total_size += m_header.capacity / 8;
    }

    // NULL bit mask.
    for (const auto &null_mask : m_header.null_masks) {
      if (null_mask) {
        total_size += m_header.capacity / 8;
      }
    }

    // TrxnJ
    if (m_header.txn_journal) {
      total_size += m_header.txn_journal->get_total_size();
    }

    // all column data size.
    for (const auto &[col_idx, cu] : m_column_units) {
      total_size += cu->get_data_size();
    }

    return total_size;
  }

  inline row_id_t get_start_row() const { return m_header.start_row; }

  inline row_id_t get_end_row() const { return m_header.end_row; }

  inline size_t get_capacity() const { return m_header.capacity; }

  inline size_t get_row_count() const { return m_header.current_rows.load(std::memory_order_acquire); }

  inline double get_delete_ratio() const { return m_header.delete_ratio; }

  // CU Management
  inline CU *get_cu(uint32_t col_idx) {
    auto it = m_column_units.find(col_idx);
    return (it != m_column_units.end()) ? it->second.get() : nullptr;
  }

  inline const CU *get_cu(uint32_t col_idx) const { return get_cu(col_idx); }

  // Serialization
  /**
   * Serialize to disk
   */
  bool serialize(std::ostream &out) const;

  /**
   * Deserialize from disk
   */
  bool deserialize(std::istream &in);

 private:
  /**
   * Initialize the header
   */
  void init_header(const Table_Metadata &table_meta) {}

  /**
   * Initialize all column units
   */
  void init_column_units(const Table_Metadata &table_meta) {}

  /**
   * Allocate a local row ID
   */
  inline row_id_t allocate_row_id() {
    size_t current = m_header.current_rows.fetch_add(1, std::memory_order_relaxed);

    if (current >= m_header.capacity) {
      m_header.current_rows.fetch_sub(1, std::memory_order_relaxed);
      m_header.status.store(Imcu_header::READ_ONLY);
      return INVALID_ROW_ID;
    }

    return static_cast<row_id_t>(current);
  }

  /**
   * Record transaction log entry
   */
  void log_transaction(row_id_t local_row_id, OPER_TYPE operation, Transaction::ID txn_id,
                       const std::unordered_map<uint32_t, RowBuffer::ColumnValue> *updates = nullptr) {}

  /**
   * Update IMCU-level statistics
   */
  void update_statistics() {}

  /**
   * Increment the internal version counter
   */
  inline void increment_version() { m_version.fetch_add(1, std::memory_order_release); }

 private:
  Imcu_header m_header;

  // key: column_id, value: CU
  std::unordered_map<uint32_t, std::unique_ptr<CU>> m_column_units;

  // Ordered CU index (for efficient batch operations)
  std::vector<CU *> m_cu_array;

  // IMCU-level read/write lock (protects header)
  mutable std::shared_mutex m_header_mutex;

  // CU creation mutex (prevents duplicate creation)
  std::mutex m_cu_creation_mutex;

  // Optimistic concurrency version counter
  std::atomic<uint64_t> m_version{0};

  // Back Reference
  RpdTable *m_owner_table;

  // Memory Management
  std::shared_ptr<Utils::MemoryPool> m_memory_pool;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCU_H__