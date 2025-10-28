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

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/imcs/imcu.h"

#include <limits.h>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type
#include "sql/sql_class.h"

#include "storage/innobase/include/mach0data.h"

#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {

void StorageIndex::update(uint32_t col_idx, double value) {
  if (col_idx >= m_num_columns) return;

  std::unique_lock lock(m_mutex);
  auto &stats = m_column_stats[col_idx];

  stats.sum.fetch_add(value, std::memory_order_relaxed);

  double current_min = stats.min_value.load(std::memory_order_relaxed);
  while (value < current_min) {
    if (stats.min_value.compare_exchange_weak(current_min, value, std::memory_order_relaxed)) {
      m_dirty.store(true, std::memory_order_relaxed);
      break;
    }
  }

  double current_max = stats.max_value.load(std::memory_order_relaxed);
  while (value > current_max) {
    if (stats.max_value.compare_exchange_weak(current_max, value, std::memory_order_relaxed)) {
      m_dirty.store(true, std::memory_order_relaxed);
      break;
    }
  }
}

void StorageIndex::update_null(uint32_t col_idx) {
  if (col_idx >= m_num_columns) return;

  std::unique_lock lock(m_mutex);
  auto &stats = m_column_stats[col_idx];

  stats.null_count.fetch_add(1, std::memory_order_relaxed);
  stats.has_null.store(true, std::memory_order_relaxed);
  m_dirty.store(true, std::memory_order_relaxed);
}

void StorageIndex::rebuild(const Imcu *imcu) {}

const StorageIndex::ColumnStats *StorageIndex::get_column_stats_snapshot(uint32_t col_idx) const {
  if (col_idx >= m_num_columns) return nullptr;

  std::shared_lock lock(m_mutex);
  return &m_column_stats[col_idx];
}

bool StorageIndex::can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const {
  for (const auto &predict : predicates) {
    auto pred = down_cast<Simple_Predicate *>(predict.get());
    uint32_t col_idx = pred->column_id;

    if (col_idx >= m_num_columns) continue;

    // Use atomic loads for fast path checking
    const double min_val = get_min_value(col_idx);
    const double max_val = get_max_value(col_idx);
    const bool has_nulls = get_has_null(col_idx);
    const size_t null_cnt = get_null_count(col_idx);

    // Check range predicates using atomic values

    switch (pred->op) {
      case PredicateOperator::EQUAL:
        // col = value
        if (pred->value.as_double() < min_val || pred->value.as_double() > max_val) {
          return true;  // Can skip
        }
        break;

      case PredicateOperator::GREATER_THAN:
        // col > value
        if (max_val <= pred->value.as_double()) {
          return true;  // Entire IMCU's max <= value, skip
        }
        break;

      case PredicateOperator::LESS_THAN:
        // col < value
        if (min_val >= pred->value.as_double()) {
          return true;  // Entire IMCU's min >= value, skip
        }
        break;

      case PredicateOperator::BETWEEN:
        // col BETWEEN min AND max
        if (max_val < pred->value.as_double() || min_val > pred->value2.as_double()) {
          return true;  // Ranges don't overlap, skip
        }
        break;

      case PredicateOperator::IS_NULL:
        // col IS NULL
        if (!has_nulls) {
          return true;  // No NULL values, skip
        }
        break;

      case PredicateOperator::IS_NOT_NULL:
        // col IS NOT NULL
        if (null_cnt == m_num_columns) {  // Using m_num_columns instead of vector size
          return true;                    // All NULL, skip
        }
        break;
      default:
        assert(false);
        break;
    }
  }
  return false;  // Cannot skip
}

double StorageIndex::estimate_selectivity(const std::vector<Predicate> &predicates) const {
  if (predicates.empty()) {
    return 1.0;
  }

  double selectivity = 1.0;

  for (const auto &pred : predicates) {
    // Each predicate estimates its own selectivity using this StorageIndex
    double pred_selectivity = pred.estimate_selectivity(this);

    // Combine selectivities (assume independence)
    selectivity *= pred_selectivity;
  }

  // Clamp result to reasonable range
  return std::max(0.0001, std::min(1.0, selectivity));
}

void StorageIndex::update_string_stats(uint32_t col_idx, const std::string &value) {
  if (col_idx >= m_num_columns) return;

  auto &stats = m_column_stats[col_idx];
  std::lock_guard lock(stats.m_string_mutex);

  if (stats.min_string.empty() || value < stats.min_string) {
    stats.min_string = value;
    m_dirty.store(true, std::memory_order_relaxed);
  }

  if (stats.max_string.empty() || value > stats.max_string) {
    stats.max_string = value;
    m_dirty.store(true, std::memory_order_relaxed);
  }
}

bool StorageIndex::serialize(std::ostream &out) const {
  std::shared_lock lock(m_mutex);

  // Write number of columns
  out.write(reinterpret_cast<const char *>(&m_num_columns), sizeof(m_num_columns));

  // Write statistics for each column (load atomic values)
  for (const auto &stats : m_column_stats) {
    double min_val = stats.min_value.load(std::memory_order_acquire);
    double max_val = stats.max_value.load(std::memory_order_acquire);
    double sum_val = stats.sum.load(std::memory_order_acquire);
    double avg_val = stats.avg.load(std::memory_order_acquire);
    size_t null_cnt = stats.null_count.load(std::memory_order_acquire);
    bool has_nulls = stats.has_null.load(std::memory_order_acquire);
    size_t distinct_cnt = stats.distinct_count.load(std::memory_order_acquire);

    out.write(reinterpret_cast<const char *>(&min_val), sizeof(min_val));
    out.write(reinterpret_cast<const char *>(&max_val), sizeof(max_val));
    out.write(reinterpret_cast<const char *>(&sum_val), sizeof(sum_val));
    out.write(reinterpret_cast<const char *>(&avg_val), sizeof(avg_val));
    out.write(reinterpret_cast<const char *>(&null_cnt), sizeof(null_cnt));
    out.write(reinterpret_cast<const char *>(&has_nulls), sizeof(has_nulls));
    out.write(reinterpret_cast<const char *>(&distinct_cnt), sizeof(distinct_cnt));

    // Serialize string data with protection
    std::lock_guard string_lock(stats.m_string_mutex);
    size_t min_str_len = stats.min_string.size();
    size_t max_str_len = stats.max_string.size();
    out.write(reinterpret_cast<const char *>(&min_str_len), sizeof(min_str_len));
    if (min_str_len > 0) {
      out.write(stats.min_string.c_str(), min_str_len);
    }
    out.write(reinterpret_cast<const char *>(&max_str_len), sizeof(max_str_len));
    if (max_str_len > 0) {
      out.write(stats.max_string.c_str(), max_str_len);
    }
  }

  return out.good();
}

bool StorageIndex::deserialize(std::istream &in) {
  std::unique_lock lock(m_mutex);

  // Read number of columns
  in.read(reinterpret_cast<char *>(&m_num_columns), sizeof(m_num_columns));
  m_column_stats.resize(m_num_columns);

#if 0
  // Read statistics for each column (store to atomic values)
  for (auto &stats : m_column_stats) {
    double min_val, max_val, sum_val, avg_val;
    size_t null_cnt, distinct_cnt;
    bool has_nulls;

    in.read(reinterpret_cast<char *>(&min_val), sizeof(min_val));
    in.read(reinterpret_cast<char *>(&max_val), sizeof(max_val));
    in.read(reinterpret_cast<char *>(&sum_val), sizeof(sum_val));
    in.read(reinterpret_cast<char *>(&avg_val), sizeof(avg_val));
    in.read(reinterpret_cast<char *>(&null_cnt), sizeof(null_cnt));
    in.read(reinterpret_cast<char *>(&has_nulls), sizeof(has_nulls));
    in.read(reinterpret_cast<char *>(&distinct_cnt), sizeof(distinct_cnt));

    stats.min_value.store(min_val);
    stats.max_value.store(max_val);
    stats.sum.store(sum_val);
    stats.avg.store(avg_val);
    stats.null_count.store(null_cnt);
    stats.has_null.store(has_nulls);
    stats.distinct_count.store(distinct_cnt);

    // Deserialize string data with protection
    std::lock_guard string_lock(stats.m_string_mutex);
    size_t min_str_len, max_str_len;
    in.read(reinterpret_cast<char *>(&min_str_len), sizeof(min_str_len));
    if (min_str_len > 0) {
      stats.min_string.resize(min_str_len);
      in.read(&stats.min_string[0], min_str_len);
    }
    in.read(reinterpret_cast<char *>(&max_str_len), sizeof(max_str_len));
    if (max_str_len > 0) {
      stats.max_string.resize(max_str_len);
      in.read(&stats.max_string[0], max_str_len);
    }
  }
#endif
  return in.good();
}

RowBuffer::ColumnValue::ColumnValue() : data(nullptr), length(0), type(MYSQL_TYPE_NULL) {
  std::memset(&flags, 0, sizeof(flags));
}

RowBuffer::ColumnValue::ColumnValue(const ColumnValue &other)
    : length(other.length), flags(other.flags), type(other.type) {
  if (other.flags.is_zero_copy) {
    // Zero-copy, directly copy pointer
    data = other.data;
  } else if (other.owned_buffer) {
    // Deep copy
    owned_buffer = std::make_unique<uchar[]>(length);
    std::memcpy(owned_buffer.get(), other.data, length);
    data = owned_buffer.get();
  } else {
    data = nullptr;
  }
}

RowBuffer::ColumnValue::ColumnValue(ColumnValue &&other) noexcept
    : data(other.data),
      length(other.length),
      flags(other.flags),
      owned_buffer(std::move(other.owned_buffer)),
      type(other.type) {
  other.data = nullptr;
  other.length = 0;
}

RowBuffer::ColumnValue &RowBuffer::ColumnValue::operator=(const ColumnValue &other) {
  if (this != &other) {
    length = other.length;
    flags = other.flags;
    type = other.type;

    if (other.flags.is_zero_copy) {
      data = other.data;
      owned_buffer.reset();
    } else if (other.owned_buffer) {
      owned_buffer = std::make_unique<uchar[]>(length);
      std::memcpy(owned_buffer.get(), other.data, length);
      data = owned_buffer.get();
    } else {
      data = nullptr;
      owned_buffer.reset();
    }
  }
  return *this;
}

RowBuffer::ColumnValue &RowBuffer::ColumnValue::operator=(ColumnValue &&other) noexcept {
  if (this != &other) {
    data = other.data;
    length = other.length;
    flags = other.flags;
    owned_buffer = std::move(other.owned_buffer);
    type = other.type;

    other.data = nullptr;
    other.length = 0;
  }
  return *this;
}

void RowBuffer::set_column_zero_copy(uint32_t col_idx, const uchar *data, size_t length, enum_field_types type) {
  if (col_idx >= m_num_columns) return;

  ColumnValue &col = m_columns[col_idx];
  col.data = data;
  col.length = length;
  col.type = type;
  col.flags.is_zero_copy = 1;
  col.flags.is_null = (length == UNIV_SQL_NULL) ? 1 : 0;
  col.owned_buffer.reset();
}

void RowBuffer::set_columns_zero_copy(const uchar **data_ptrs, const size_t *lengths) {
  for (size_t i = 0; i < m_num_columns; i++) {
    size_t len = lengths ? lengths[i] : 0;
    set_column_zero_copy(i, data_ptrs[i], len);
  }
}

void RowBuffer::set_column_copy(uint32_t col_idx, const uchar *data, size_t length, enum_field_types type) {
  if (col_idx >= m_num_columns) return;

  ColumnValue &col = m_columns[col_idx];
  col.length = length;
  col.type = type;
  col.flags.is_zero_copy = 0;
  col.flags.is_null = (length == UNIV_SQL_NULL || data == nullptr) ? 1 : 0;

  if (!col.flags.is_null) {
    // Allocate buffer and copy
    col.owned_buffer = std::make_unique<uchar[]>(length);
    std::memcpy(col.owned_buffer.get(), data, length);
    col.data = col.owned_buffer.get();

    m_is_all_zero_copy = false;
  } else {
    col.data = nullptr;
    col.owned_buffer.reset();
  }
}

int64_t RowBuffer::get_column_int(uint32_t col_idx) const {
  const ColumnValue *col = get_column(col_idx);
  if (!col || col->flags.is_null) return 0;

  switch (col->type) {
    case MYSQL_TYPE_TINY:
      return *reinterpret_cast<const int8_t *>(col->data);
    case MYSQL_TYPE_SHORT:
      return *reinterpret_cast<const int16_t *>(col->data);
    case MYSQL_TYPE_LONG:
      return *reinterpret_cast<const int32_t *>(col->data);
    case MYSQL_TYPE_LONGLONG:
      return *reinterpret_cast<const int64_t *>(col->data);
    default:
      return 0;
  }
}

double RowBuffer::get_column_double(uint32_t col_idx) const {
  const ColumnValue *col = get_column(col_idx);
  if (!col || col->flags.is_null) return 0.0;

  switch (col->type) {
    case MYSQL_TYPE_FLOAT:
      return *reinterpret_cast<const float *>(col->data);
    case MYSQL_TYPE_DOUBLE:
      return *reinterpret_cast<const double *>(col->data);
    default:
      return 0.0;
  }
}

size_t RowBuffer::get_column_string(uint32_t col_idx, char *buffer, size_t buffer_size) const {
  const ColumnValue *col = get_column(col_idx);
  if (!col || col->flags.is_null || !col->data) {
    if (buffer_size > 0) buffer[0] = '\0';
    return 0;
  }

  size_t copy_len = std::min(col->length, buffer_size - 1);
  std::memcpy(buffer, col->data, copy_len);
  buffer[copy_len] = '\0';

  return copy_len;
}

void RowBuffer::clear() {
  m_row_id = INVALID_ROW_ID;

  for (auto &col : m_columns) {
    col.data = nullptr;
    col.length = 0;
    col.flags.is_null = 1;
    col.owned_buffer.reset();
  }

  m_is_all_zero_copy = true;
}

void RowBuffer::convert_to_owned() {
  if (m_is_all_zero_copy) {
    for (auto &col : m_columns) {
      if (col.flags.is_zero_copy && !col.flags.is_null) {
        // Convert to copy mode
        auto buffer = std::make_unique<uchar[]>(col.length);
        std::memcpy(buffer.get(), col.data, col.length);

        col.data = buffer.get();
        col.owned_buffer = std::move(buffer);
        col.flags.is_zero_copy = 0;
      }
    }
    m_is_all_zero_copy = false;
  }
}

bool RowBuffer::serialize(std::ostream &out) const {
  // Write row ID
  out.write(reinterpret_cast<const char *>(&m_row_id), sizeof(m_row_id));

  // Write number of columns
  out.write(reinterpret_cast<const char *>(&m_num_columns), sizeof(m_num_columns));

  // Write each column's data
  for (const auto &col : m_columns) {
    // Write length
    out.write(reinterpret_cast<const char *>(&col.length), sizeof(col.length));

    // Write flags
    out.write(reinterpret_cast<const char *>(&col.flags), sizeof(col.flags));

    // Write type
    out.write(reinterpret_cast<const char *>(&col.type), sizeof(col.type));

    // Write data
    if (!col.flags.is_null && col.data) {
      out.write(reinterpret_cast<const char *>(col.data), col.length);
    }
  }

  return out.good();
}

bool RowBuffer::deserialize(std::istream &in) {
  // Read row ID
  in.read(reinterpret_cast<char *>(&m_row_id), sizeof(m_row_id));

  // Read number of columns
  size_t num_cols;
  in.read(reinterpret_cast<char *>(&num_cols), sizeof(num_cols));

  if (num_cols != m_num_columns) {
    m_num_columns = num_cols;
    m_columns.resize(num_cols);
  }

  // Read each column's data
  for (auto &col : m_columns) {
    // Read length
    in.read(reinterpret_cast<char *>(&col.length), sizeof(col.length));

    // Read flags
    in.read(reinterpret_cast<char *>(&col.flags), sizeof(col.flags));

    // Read type
    in.read(reinterpret_cast<char *>(&col.type), sizeof(col.type));

    // Read data
    if (!col.flags.is_null && col.length > 0) {
      col.owned_buffer = std::make_unique<uchar[]>(col.length);
      in.read(reinterpret_cast<char *>(col.owned_buffer.get()), col.length);
      col.data = col.owned_buffer.get();
      col.flags.is_zero_copy = 0;
    } else {
      col.data = nullptr;
      col.owned_buffer.reset();
    }
  }

  m_is_all_zero_copy = false;

  return in.good();
}

bool RowBuffer::copy_to_mysql_fields(Field **fields, size_t num_fields) const {
  if (num_fields != m_num_columns) return false;

  for (size_t i = 0; i < num_fields; i++) {
    const ColumnValue &col = m_columns[i];
    Field *field = fields[i];

    if (col.flags.is_null) {
      field->set_null();
    } else {
      field->set_notnull();

      // Set value according to field type
      switch (field->type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
          field->store(*reinterpret_cast<const longlong *>(col.data), false);
          break;

        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
          field->store(*reinterpret_cast<const double *>(col.data));
          break;

        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_BLOB:
          field->store(reinterpret_cast<const char *>(col.data), col.length, field->charset());
          break;

        default:
          // General handling: direct memory copy
          std::memcpy(field->field_ptr(), col.data, std::min(col.length, (size_t)field->pack_length()));
          break;
      }
    }
  }

  return true;
}

bool RowBuffer::copy_from_mysql_fields(Field **fields, size_t num_fields) {
  if (num_fields != m_num_columns) return false;

  for (size_t idx = 0; idx < num_fields; idx++) {
    Field *fld = fields[idx];

    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};

    if (fld->is_null()) {
      set_column_null(idx);
    } else {
      // Get field data
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          data_ptr = fld->field_ptr();
          // TODO: BLOB data maybe not in the page. stores off the page.
          auto bfld = down_cast<Field_blob *>(fld);
          uint pack_len = bfld->pack_length_no_ptr();
          switch (pack_len) {
            case 1:
              data_len = *data_ptr;
              break;
            case 2:
              data_len = uint2korr(data_ptr);
              break;
            case 3:
              data_len = uint3korr(data_ptr);
              break;
            case 4:
              data_len = uint4korr(data_ptr);
              break;
          }
          // Advance past length prefix
          data_ptr += pack_len;

          // For BLOBs, the data_ptr now points to a pointer to the actual blob data
          uchar *blob_ptr = nullptr;
          memcpy(&blob_ptr, data_ptr, sizeof(uchar *));
          data_ptr = blob_ptr;
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING: {
          extra_offset = (fld->field_length > 256 ? 2 : 1);
          data_ptr = fld->field_ptr() + extra_offset;
          if (extra_offset == 1)
            data_len = mach_read_from_1(fld->field_ptr());
          else if (extra_offset == 2)
            data_len = mach_read_from_2_little_endian(fld->field_ptr());
        } break;
        default: {
          data_ptr = fld->field_ptr();
          data_len = fld->pack_length();
        } break;
      }
      // Copy mode (safe)
      set_column_copy(idx, data_ptr, data_len, fld->type());
    }
  }

  return true;
}

bool RowBuffer::copy_from_mysql_fields(Field **fields, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                                       ulong *null_byte_offsets, ulong *null_bitmasks) {
  if (n_cols != m_num_columns) return false;

  auto data_len{0u};
  uchar *data_ptr{nullptr};

  for (auto col_ind = 0u; col_ind < n_cols; col_ind++) {
    auto fld = fields[col_ind];
    data_ptr = rowdata + col_offsets[col_ind];
    auto is_null = is_field_null(col_ind, rowdata, null_byte_offsets, null_bitmasks);
    if (is_null) {
      set_column_null(col_ind);
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          // TODO: BLOB data maybe not in the page. stores off the page.
          uint pack_len = down_cast<Field_blob *>(fld)->pack_length_no_ptr();
          switch (pack_len) {
            case 1:
              data_len = *data_ptr;
              break;
            case 2:
              data_len = uint2korr(data_ptr);
              break;
            case 3:
              data_len = uint3korr(data_ptr);
              break;
            case 4:
              data_len = uint4korr(data_ptr);
              break;
          }
          // Advance past length prefix
          data_ptr += pack_len;

          // For BLOBs, the data_ptr now points to a pointer to the actual blob data
          uchar *blob_ptr = nullptr;
          memcpy(&blob_ptr, data_ptr, sizeof(uchar *));
          data_ptr = blob_ptr;
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING: {
          auto extra_offset = (fld->field_length > 256 ? 2 : 1);
          if (extra_offset == 1)
            data_len = mach_read_from_1(data_ptr);
          else if (extra_offset == 2)
            data_len = mach_read_from_2_little_endian(data_ptr);
          data_ptr = data_ptr + ptrdiff_t(extra_offset);
        } break;
        default: {
          data_len = fld->pack_length();
        } break;
      }
      // Copy mode (safe)
      set_column_copy(col_ind, data_ptr, data_len, fld->type());
    }
  }
  return true;
}

void RowBuffer::dump(std::ostream &out) const {
  out << "Row " << m_row_id << " (" << m_num_columns << " columns):\n";

  for (size_t i = 0; i < m_num_columns; i++) {
    const ColumnValue &col = m_columns[i];

    out << "  Column " << i << ": ";

    if (col.flags.is_null) {
      out << "NULL\n";
    } else {
      out << "length=" << col.length << ", zero_copy=" << (col.flags.is_zero_copy ? "yes" : "no")
          << ", type=" << static_cast<int>(col.type);

      // Print partial data (first 16 bytes)
      if (col.data) {
        out << ", data=[";
        size_t print_len = std::min(col.length, size_t(16));
        for (size_t j = 0; j < print_len; j++) {
          out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(col.data[j]) << " ";
        }
        if (col.length > 16) out << "...";
        out << "]";
      }

      out << "\n";
    }
  }
}

bool RowBuffer::validate() const {
  for (const auto &col : m_columns) {
    // Check pointer validity in zero-copy mode
    if (col.flags.is_zero_copy && !col.flags.is_null && !col.data) {
      return false;
    }

    // Check buffer consistency in copy mode
    if (!col.flags.is_zero_copy && !col.flags.is_null) {
      if (col.owned_buffer && col.data != col.owned_buffer.get()) {
        return false;
      }
    }

    // Check NULL flag consistency
    if (col.flags.is_null && col.data != nullptr) {
      return false;
    }
  }

  return true;
}

RowDirectory::RowDirectory(RowDirectory &&other) noexcept
    : m_capacity(other.m_capacity),
      m_enable_column_offsets(other.m_enable_column_offsets),
      m_num_columns(other.m_num_columns),
      m_total_data_size(other.m_total_data_size.load()),
      m_compressed_data_size(other.m_compressed_data_size.load()),
      m_overflow_count(other.m_overflow_count.load()) {
  // Transfer ownership of unique_ptr members
  m_entries = std::move(other.m_entries);
  m_column_offset_tables = std::move(other.m_column_offset_tables);

  // Reset the source object
  other.m_capacity = 0;
  other.m_num_columns = 0;
  other.m_total_data_size = 0;
  other.m_compressed_data_size = 0;
  other.m_overflow_count = 0;
}

RowDirectory &RowDirectory::operator=(RowDirectory &&other) noexcept {
  if (this != &other) {
    m_capacity = other.m_capacity;
    m_num_columns = other.m_num_columns;
    m_enable_column_offsets = other.m_enable_column_offsets;
    m_total_data_size = other.m_total_data_size.load();
    m_compressed_data_size = other.m_compressed_data_size.load();
    m_overflow_count = other.m_overflow_count.load();

    // Transfer ownership
    m_entries = std::move(other.m_entries);
    m_column_offset_tables = std::move(other.m_column_offset_tables);

    // Reset the source object
    other.m_capacity = 0;
    other.m_num_columns = 0;
    other.m_total_data_size = 0;
    other.m_compressed_data_size = 0;
    other.m_overflow_count = 0;
  }
  return *this;
}

void RowDirectory::set_row_entry(row_id_t row_id, uint32_t offset, uint32_t length, bool is_compressed) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);

  RowEntry &entry = m_entries[row_id];
  entry.offset = offset;
  entry.length = length;
  entry.flags.is_compressed = is_compressed ? 1 : 0;
  entry.checksum = compute_checksum(offset, length);

  m_total_data_size.fetch_add(length);
  if (is_compressed) {
    m_compressed_data_size.fetch_add(length);
  }
}

const RowDirectory::RowEntry *RowDirectory::get_row_entry(row_id_t row_id) const {
  if (row_id >= m_capacity) return nullptr;

  std::shared_lock lock(m_mutex);
  return &m_entries[row_id];
}

void RowDirectory::mark_deleted(row_id_t row_id) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.is_deleted = 1;
}

void RowDirectory::mark_has_null(row_id_t row_id) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.has_null = 1;
}

void RowDirectory::mark_overflow(row_id_t row_id) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.is_overflow = 1;
  m_overflow_count.fetch_add(1);
}

void RowDirectory::build_column_offset_table(row_id_t row_id, const std::vector<uint16_t> &column_offsets,
                                             const std::vector<uint16_t> &column_lengths) {
  if (!m_enable_column_offsets || row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);

  auto table = std::make_unique<RowDirectory::ColumnOffsetTable>(m_num_columns);
  table->column_offsets = column_offsets;
  table->column_lengths = column_lengths;

  m_column_offset_tables[row_id] = std::move(table);
}

const RowDirectory::ColumnOffsetTable *RowDirectory::get_column_offset_table(row_id_t row_id) const {
  if (!m_enable_column_offsets || row_id >= m_capacity) return nullptr;

  std::shared_lock lock(m_mutex);

  auto it = m_column_offset_tables.find(row_id);
  return (it != m_column_offset_tables.end()) ? it->second.get() : nullptr;
}

uint16_t RowDirectory::get_column_offset(row_id_t row_id, uint32_t col_idx) const {
  const RowDirectory::ColumnOffsetTable *table = get_column_offset_table(row_id);

  if (table && col_idx < table->column_offsets.size()) {
    return table->column_offsets[col_idx];
  }

  return UINT16_MAX;
}

uint16_t RowDirectory::get_column_length(row_id_t row_id, uint32_t col_idx) const {
  const RowDirectory::ColumnOffsetTable *table = get_column_offset_table(row_id);

  if (table && col_idx < table->column_lengths.size()) {
    return table->column_lengths[col_idx];
  }

  return 0;
}

void RowDirectory::get_batch_offsets(row_id_t start_row, size_t count, uint32_t *offsets, uint32_t *lengths) const {
  std::shared_lock lock(m_mutex);

  size_t end = std::min(start_row + count, m_capacity);

  for (size_t i = start_row; i < end; i++) {
    size_t idx = i - start_row;
    offsets[idx] = m_entries[i].offset;
    lengths[idx] = m_entries[i].length;
  }
}

void RowDirectory::update_compression_stats(row_id_t row_id, size_t original_size, size_t compressed_size) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);

  m_entries[row_id].flags.is_compressed = 1;
  m_entries[row_id].length = compressed_size;

  m_compressed_data_size.fetch_add(compressed_size);
}

double RowDirectory::get_compression_ratio() const {
  size_t total = m_total_data_size.load();
  size_t compressed = m_compressed_data_size.load();

  if (total == 0) return 0.0;

  return static_cast<double>(compressed) / total;
}

size_t RowDirectory::get_directory_size() const {
  size_t base_size = m_capacity * sizeof(RowEntry);

  std::shared_lock lock(m_mutex);

  size_t offset_table_size = 0;
  for (const auto &[row_id, table] : m_column_offset_tables) {
    offset_table_size += table->column_offsets.size() * sizeof(uint16_t);
    offset_table_size += table->column_lengths.size() * sizeof(uint16_t);
  }

  return base_size + offset_table_size;
}

bool RowDirectory::validate() const {
  std::shared_lock lock(m_mutex);

  for (size_t i = 0; i < m_capacity; i++) {
    const RowEntry &entry = m_entries[i];

    // Verify checksum
    uint32_t expected_checksum = compute_checksum(entry.offset, entry.length);
    if (entry.checksum != expected_checksum) {
      return false;
    }
  }

  return true;
}

void RowDirectory::dump_summary(std::ostream &out) const {
  std::shared_lock lock(m_mutex);

  out << "Row Directory Summary:\n";
  out << "  Capacity: " << m_capacity << "\n";
  out << "  Total Data Size: " << m_total_data_size.load() << " bytes\n";
  out << "  Compressed Data Size: " << m_compressed_data_size.load() << " bytes\n";
  out << "  Compression Ratio: " << get_compression_ratio() << "\n";
  out << "  Overflow Count: " << m_overflow_count.load() << "\n";
  out << "  Directory Size: " << get_directory_size() << " bytes\n";
  out << "  Column Offset Tables: " << m_column_offset_tables.size() << "\n";
}

std::unique_ptr<RowDirectory> RowDirectory::clone() const {
  auto new_dir = std::make_unique<RowDirectory>(m_capacity, m_num_columns, m_enable_column_offsets);

  std::shared_lock lock(m_mutex);

  // Copy row entries
  for (size_t i = 0; i < m_capacity; i++) {
    new_dir->m_entries[i] = m_entries[i];
  }

  // Copy column offset tables
  for (const auto &[row_id, table] : m_column_offset_tables) {
    auto new_table = std::make_unique<RowDirectory::ColumnOffsetTable>(m_num_columns);
    new_table->column_offsets = table->column_offsets;
    new_table->column_lengths = table->column_lengths;
    new_dir->m_column_offset_tables[row_id] = std::move(new_table);
  }

  // Copy atomic values
  new_dir->m_total_data_size.store(m_total_data_size.load());
  new_dir->m_compressed_data_size.store(m_compressed_data_size.load());
  new_dir->m_overflow_count.store(m_overflow_count.load());

  return new_dir;
}

Imcu::Imcu(RpdTable *owner, Table_Metadata &table_meta, row_id_t start_row, size_t capacity,
           std::shared_ptr<Utils::MemoryPool> mem_pool)
    : m_owner_table(owner), m_memory_pool(mem_pool) {
  m_header.imcu_id = owner->meta().total_imcus.fetch_add(1);
  m_header.start_row = start_row;
  m_header.end_row = start_row + capacity;
  m_header.capacity = capacity;
  m_header.current_rows = 0;
  m_header.created_at = std::chrono::system_clock::now();
  m_header.last_modified = std::chrono::system_clock::now();

  // to indicate which row is deleted or not. row-level shared.
  m_header.del_mask = std::make_unique<bit_array_t>(m_header.capacity);

  for (auto &fld_meta : owner->meta().fields) {
    if (fld_meta.is_secondary_field) {
      auto cu_fld = std::make_unique<CU>(this, fld_meta, fld_meta.field_id, m_header.capacity, m_memory_pool);
      m_column_units.emplace(fld_meta.field_id, std::move(cu_fld));
      m_cu_array.push_back(m_column_units[fld_meta.field_id].get());

      m_header.null_masks.emplace_back(std::make_unique<bit_array_t>(m_header.capacity));
    } else {  // NOT SECONDARY FIELD.
      m_column_units.emplace(fld_meta.field_id, nullptr);
      m_cu_array.push_back(nullptr);
      m_header.null_masks.emplace_back(nullptr);
    }
  }

  // create transaction journal associated with this imuc.
  m_header.txn_journal = std::make_unique<TransactionJournal>(m_header.capacity);

  // create storage index associated with this imcu.
  m_header.storage_index = std::make_unique<StorageIndex>(table_meta.num_columns);

  // create row dir index associated with this imcu.
  m_header.row_directory = std::make_unique<RowDirectory>(m_header.capacity, table_meta.num_columns);
}

Imcu::~Imcu() {}

row_id_t Imcu::insert_row(const Rapid_load_context *context, const RowBuffer &row_data) {
  // 1. allocate local row_id.
  row_id_t local_row_id = allocate_row_id();

  if (local_row_id == INVALID_ROW_ID) {  // IMCU full.
    return INVALID_ROW_ID;
  }

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;

  // 2. record Transaction_Journal.
  {
    std::unique_lock lock(m_header_mutex);

    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_INSERT);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    m_header.txn_journal->add_entry(std::move(entry));

    m_header.insert_count.fetch_add(1);
  }

  // 3. write to each column.
  for (size_t col_idx = 0; col_idx < row_data.get_num_columns(); col_idx++) {
    if (!m_cu_array[col_idx]) continue;  // means is `NOT_SECONDARY` field.

    auto row_col_data = row_data.get_column(col_idx);
    // dealing with NULL
    if (row_col_data->flags.is_null) {
      assert(row_col_data->data == nullptr);
      assert(m_header.null_masks[col_idx].get());

      std::unique_lock lock(m_header_mutex);
      Utils::Util::bit_array_set(m_header.null_masks[col_idx].get(), local_row_id);
    }

    // write data（dont create version due to its insertion）
    m_cu_array[col_idx]->write(context, local_row_id, row_col_data->data, row_col_data->length);

    // update Storage Index
    if (row_col_data->data && is_numeric_type(row_col_data->type)) {
      auto src_fld = m_owner_table->meta().fields[col_idx].source_fld;
      double numeric_val = Utils::Util::get_field_numeric<double>(src_fld, row_col_data->data, nullptr);
      m_header.storage_index->update(col_idx, numeric_val);
    }
  }

  increment_version();

  return local_row_id;
}

int Imcu::delete_row(const Rapid_load_context *context, row_id_t local_row_id) {
  // 1. boundary check.
  if (local_row_id >= m_header.current_rows.load()) return HA_ERR_KEY_NOT_FOUND;

  // 2. check whether it deleted or not.
  {
    std::shared_lock lock(m_header_mutex);
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id))
      return HA_ERR_RECORD_DELETED;  // alread deleted.
  }

  // 3. record transaction journal.
  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;  // if committed.

  {
    std::unique_lock lock(m_header_mutex);

    // 3.1 create TxnJ record.
    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_UPDATE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    // 3.2 add entry.
    m_header.txn_journal->add_entry(std::move(entry));

    // 3.3 mark it deleted.
    Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);

    // 3.4 update statistics.
    m_header.delete_count.fetch_add(1);
    m_header.delete_ratio = static_cast<double>(m_header.delete_count.load()) / m_header.current_rows.load();
  }

  // 4. increase the version counter（optimistic concurrent）
  increment_version();

  // 5. update Storage Index（mark it need to rebuild）
  m_header.storage_index->mark_dirty();

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Imcu::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &local_row_ids) {
  if (local_row_ids.empty()) return 0;

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;

  std::unique_lock lock(m_header_mutex);

  size_t deleted = 0;

  for (row_id_t local_row_id : local_row_ids) {
    // boundary check.
    if (local_row_id >= m_header.current_rows.load()) continue;

    // already deleted or not.
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) continue;

    // build up a TxnJ
    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_DELETE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    m_header.txn_journal->add_entry(std::move(entry));

    // to set deleted flag.
    Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);

    deleted++;
  }

  // update statistics.
  m_header.delete_count.fetch_add(deleted);
  m_header.delete_ratio = static_cast<double>(m_header.delete_count.load()) / m_header.current_rows.load();

  increment_version();
  m_header.storage_index->mark_dirty();

  return deleted;
}

int Imcu::update_row(const Rapid_load_context *context, row_id_t local_row_id,
                     const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates) {
  // 1. check boundary.
  if (local_row_id >= m_header.current_rows.load()) return HA_ERR_KEY_NOT_FOUND;

  // 2. check deleted or not.
  {
    std::shared_lock lock(m_header_mutex);
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) return HA_ERR_RECORD_DELETED;
  }

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;

  // 3. record row level TxnJ.
  {
    std::unique_lock lock(m_header_mutex);

    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_UPDATE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    // mark update bit
    for (const auto &[col_idx, value] : updates) entry.modified_columns.set(col_idx);

    m_header.txn_journal->add_entry(std::move(entry));

    m_header.update_count.fetch_add(1);
  }

  // 4. Create column-level versions for each modified column and write new values
  // Key point: Only operate on modified columns! Do not touch unmodified columns at all
  for (const auto &[col_idx, new_value] : updates) {
    CU *cu = get_cu(col_idx);
    if (!cu) continue;

    // CU-leve update（create row-level version）
    cu->update(context, local_row_id, new_value.data, new_value.length);
  }

  // 5. update Storage Index（only apply changed column）
  for (const auto &[col_idx, new_value] : updates) {
    CU *cu = get_cu(col_idx);
    if (!cu) continue;

    assert(cu->get_source_field()->type() == cu->get_type());
    if (is_numeric_type(cu->get_type())) {
      double numeric_val = Utils::Util::get_field_numeric<double>(cu->get_source_field(), new_value.data, nullptr);
      m_header.storage_index->update(col_idx, numeric_val);
    }
  }

  increment_version();

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Imcu::scan(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                  const std::vector<uint32_t> &projection, RowCallback callback) {
  size_t num_rows = m_header.current_rows.load();
  size_t scanned = 0;

  std::vector<const uchar *> row_buffer(projection.size());

  // visibility mask.
  bit_array_t visibility_mask(SHANNON_VECTOR_WIDTH);

  // batch vector process.
  for (size_t start = 0; start < num_rows; start += SHANNON_VECTOR_WIDTH) {
    size_t end = std::min(start + SHANNON_VECTOR_WIDTH, num_rows);
    size_t batch_size = end - start;

    // 1. Batch Visibility Check (Core: One-time determination benefits all columns)
    check_visibility_batch(context, static_cast<row_id_t>(start), batch_size, visibility_mask);

    // 2. Apply predicate filtering on visible rows
    std::vector<row_id_t> matching_rows;
    matching_rows.reserve(batch_size);

    for (size_t i = 0; i < batch_size; i++) {
      if (!Utils::Util::bit_array_get(&visibility_mask, i)) {
        continue;  // invisible，skip.
      }

      row_id_t local_row_id = start + i;

      // apply prediction.
      bool match = true;
      for (const auto &pred : predicates) {
        auto col_id = down_cast<Simple_Predicate *>(pred.get())->column_id;
        CU *cu = get_cu(col_id);
        if (!cu) continue;

        const uchar *value = cu->get_data_address(local_row_id);

        if (!down_cast<Simple_Predicate *>(pred.get())->evaluate(&value, col_id)) {
          match = false;
          break;
        }
      }

      if (match) {
        matching_rows.push_back(local_row_id);
      }
    }

    // 3. Read projection columns of matched rows
    for (row_id_t local_row_id : matching_rows) {
      // read the loaded CUs.
      for (size_t i = 0; i < projection.size(); i++) {
        uint32_t col_idx = projection[i];
        CU *cu = get_cu(col_idx);
        row_buffer[i] = cu->get_data_address(local_row_id);  // return data addr directly, no data cpy.
      }

      // 4. callback.
      row_id_t global_row_id = m_header.start_row + local_row_id;
      callback(global_row_id, row_buffer);

      scanned++;
      context->rows_returned++;

      // check the LIMIT options.
      if (context->limit > 0 && context->rows_returned >= context->limit) {
        return scanned;
      }
    }
  }

  return scanned;
}

bool Imcu::is_row_visible(Rapid_scan_context *context, row_id_t local_row_id, Transaction::ID reader_txn_id,
                          uint64_t reader_scn) const {
  return 0;
}

void Imcu::check_visibility_batch(Rapid_scan_context *context, row_id_t start_row, size_t count,
                                  bit_array_t &visibility_mask) const {
  std::shared_lock lock(m_header_mutex);

  // Using Transaction Journal for Batch Visibility Determination
  m_header.txn_journal->check_visibility_batch(start_row, count, context->m_extra_info.m_trxid,
                                               context->m_extra_info.m_scn, visibility_mask);

  // filter out the deleted rows.
  for (size_t i = 0; i < count; i++) {
    if (Utils::Util::bit_array_get(&visibility_mask, i)) {
      row_id_t local_row_id = start_row + i;

      // check delete mask bit.
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) {
        // delete，Need to further check if it is visible in the snapshot.
        if (!m_header.txn_journal->is_row_visible(local_row_id, context->m_extra_info.m_trxid,
                                                  context->m_extra_info.m_scn)) {
          Utils::Util::bit_array_reset(&visibility_mask, i);
        }
      }
    }
  }
}

bool Imcu::read_row(Rapid_scan_context *context, row_id_t local_row_id, const std::vector<uint32_t> &col_indices,
                    RowBuffer &output) {
  return 0;
}

bool Imcu::can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const {
  std::shared_lock lock(m_header_mutex);
  return m_header.storage_index->can_skip_imcu(predicates);
}

void Imcu::update_storage_index() {
  if (!m_header.storage_index) {
    return;
  }

  std::unique_lock lock(m_header_mutex);

  size_t num_rows = m_header.current_rows.load(std::memory_order_acquire);
  if (num_rows == 0) {
    return;
  }

  // Iterate through all columns to update statistics
  for (auto &[col_idx, cu] : m_column_units) {
    if (!cu) continue;  // Skip NOT_SECONDARY fields

    Field *source_field = cu->get_source_field();
    if (!source_field) continue;

    enum_field_types field_type = cu->get_type();
    bool is_numeric = is_numeric_type(field_type);

    // Reset column statistics before rebuilding
    m_header.storage_index->get_column_stats_snapshot(col_idx);

    // Traverse all valid (non-deleted) rows
    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
      // Skip deleted rows
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), row_idx)) {
        continue;
      }

      // Check if NULL
      if (m_header.null_masks[col_idx] && Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), row_idx)) {
        m_header.storage_index->update_null(col_idx);
        continue;
      }

      // Get column data address
      const uchar *data = cu->get_data_address(row_idx);
      if (!data) continue;

      // Update statistics based on data type
      if (is_numeric) {
        // Extract numeric value
        double numeric_val = Utils::Util::get_field_numeric<double>(source_field, data, nullptr);
        m_header.storage_index->update(col_idx, numeric_val);
      } else {
        // Handle string types
        switch (field_type) {
          case MYSQL_TYPE_VARCHAR:
          case MYSQL_TYPE_VAR_STRING:
          case MYSQL_TYPE_STRING: {
            // Extract string value
            size_t str_len = 0;
            const char *str_data = nullptr;

            // Determine length prefix size
            size_t length_bytes = (source_field->field_length > 256) ? 2 : 1;

            if (length_bytes == 1) {
              str_len = mach_read_from_1(data);
              str_data = reinterpret_cast<const char *>(data + 1);
            } else {
              str_len = mach_read_from_2_little_endian(data);
              str_data = reinterpret_cast<const char *>(data + 2);
            }

            if (str_data && str_len > 0) {
              std::string str_value(str_data, str_len);
              m_header.storage_index->update_string_stats(col_idx, str_value);
            }
            break;
          }

          case MYSQL_TYPE_BLOB:
          case MYSQL_TYPE_TINY_BLOB:
          case MYSQL_TYPE_MEDIUM_BLOB:
          case MYSQL_TYPE_LONG_BLOB: {
            // For BLOB types, extract length and data pointer
            auto blob_field = down_cast<Field_blob *>(source_field);
            uint pack_len = blob_field->pack_length_no_ptr();

            size_t blob_len = 0;
            switch (pack_len) {
              case 1:
                blob_len = *data;
                break;
              case 2:
                blob_len = uint2korr(data);
                break;
              case 3:
                blob_len = uint3korr(data);
                break;
              case 4:
                blob_len = uint4korr(data);
                break;
            }

            // Get actual blob data pointer
            const uchar *blob_ptr = nullptr;
            memcpy(&blob_ptr, data + pack_len, sizeof(uchar *));

            if (blob_ptr && blob_len > 0) {
              std::string blob_value(reinterpret_cast<const char *>(blob_ptr),
                                     std::min(blob_len, size_t(256)));  // Limit for statistics
              m_header.storage_index->update_string_stats(col_idx, blob_value);
            }
            break;
          }

          default:
            // For other types, treat as binary and skip string statistics
            break;
        }
      }
    }
  }

  // Clear dirty flag after update
  m_header.storage_index->clear_dirty();

  // Update last modified time
  m_header.last_modified = std::chrono::system_clock::now();
}

size_t Imcu::garbage_collect(uint64_t min_active_scn) {
  size_t freed = 0;

  // 1. purege TxnJ.
  freed += m_header.txn_journal->purge(min_active_scn);

  // 2. clear version of every column.
  for (auto &[col_idx, cu] : m_column_units) {
    freed += cu->purge_versions(nullptr, min_active_scn);
  }

  // 3. update statistics.
  m_header.version_count = 0;  // estimate_version_count();
  m_header.last_gc_time = std::chrono::system_clock::now();

  return freed;
}

Imcu *Imcu::compact() {
  size_t num_rows = m_header.current_rows.load();

  // 1. calc un-deleted # of rows.
  std::vector<row_id_t> valid_rows;
  valid_rows.reserve(num_rows);

  {
    std::shared_lock lock(m_header_mutex);

    for (size_t i = 0; i < num_rows; i++) {
      if (!Utils::Util::bit_array_get(m_header.del_mask.get(), i)) {
        valid_rows.push_back(i);
        valid_rows.push_back(i);
      }
    }
  }

  // 2. create a new IMCU.
  auto new_imcu = std::make_shared<Imcu>(m_owner_table, m_owner_table->meta(), m_header.start_row, valid_rows.size(),
                                         m_memory_pool);

  // 3. cp the un-deleted rows.
  for (size_t new_row_id = 0; new_row_id < valid_rows.size(); new_row_id++) {
    row_id_t old_row_id = valid_rows[new_row_id];

    // cp the columns data.
    for (auto &[col_idx, old_cu] : m_column_units) {
      CU *new_cu = new_imcu->get_cu(col_idx);

      // read old data.
      uchar buffer[MAX_FIELD_WIDTH];
      size_t len = old_cu->read(nullptr, old_row_id, buffer);

      // write to the new place.
      new_cu->write(nullptr, new_row_id, len == UNIV_SQL_NULL ? nullptr : buffer, len);

      // cp null bits mask.
      if (Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), old_row_id)) {
        Utils::Util::bit_array_set(new_imcu->m_header.null_masks[col_idx].get(), new_row_id);
      }
    }
  }

  // 4. re-build Storage Index.
  new_imcu->update_storage_index();

  // 5. update statistic.
  new_imcu->m_header.current_rows.store(valid_rows.size());
  new_imcu->m_header.delete_count.store(0);
  new_imcu->m_header.delete_ratio = 0.0;

  return new_imcu.get();
}

bool Imcu::serialize(std::ostream &out) const { return false; }

bool Imcu::deserialize(std::istream &in) { return false; }

}  // namespace Imcs
}  // namespace ShannonBase
