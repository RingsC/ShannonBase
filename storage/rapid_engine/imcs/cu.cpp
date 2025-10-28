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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/cu.h"

#include <limits.h>
#include <iostream>
#include <random>
#include <regex>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
// SHANNON_THREAD_LOCAL Cu::LocalDataBuffer Cu::m_buff;
Cu::Cu(RapidTable *owner, const Field *field) {
  // static_assert(alignof(m_buff) >= CACHE_LINE_SIZE, "Alignment failed");
  ut_a(field && !field->is_flag_set(NOT_SECONDARY_FLAG));

  m_header = std::make_unique<Cu_header>();
  if (!m_header) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu header allocation failed");
    return;
  }

  // TODO: be aware of freeing the cloned object here.
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  init_header(m_header.get(), owner, m_header->m_source_fld);
  init_body(m_header->m_source_fld);
}

Cu::Cu(RapidTable *owner, const Field *field, std::string name) {
  // TODO: be aware of freeing the cloned object here.
  m_header = std::make_unique<Cu_header>();
  if (!m_header) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu header allocation failed");
    return;
  }
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  init_header(m_header.get(), owner, m_header->m_source_fld);
  init_body(m_header->m_source_fld);
  m_cu_key = name;
}

Cu::~Cu() {
  if (m_chunks.size()) m_chunks.clear();
}

void Cu::init_header(Cu_header *header, RapidTable *owner, const Field *field) {
  {
    header->m_owner = owner;
    header->m_type = field->type();
    header->m_width = field->pack_length();
    header->m_charset = field->charset();
    ut_a(header->m_source_fld->table->file->ref_length == field->table->file->ref_length);
    header->m_key_len.store(header->m_source_fld->table->file->ref_length);
  }
  m_cu_key.append(field->table->s->db.str)
      .append(":")
      .append(field->table->s->table_name.str)
      .append(":")
      .append(field->field_name);

  std::string comment(field->comment.str);
  std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
  const char *const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
  std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs | std::regex_constants::icase);

  if (std::regex_search(comment.c_str(), column_encoding_patt)) {
    if (comment.find("SORTED") != std::string::npos)
      header->m_encoding_type = Compress::Encoding_type::SORTED;
    else if (comment.find("VARLEN") != std::string::npos)
      header->m_encoding_type = Compress::Encoding_type::VARLEN;
  } else
    header->m_encoding_type = Compress::Encoding_type::NONE;

  header->m_local_dict = std::make_unique<Compress::Dictionary>(header->m_encoding_type);
  if (!header->m_local_dict) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu dictionary allocation failed");
    return;
  }
}

void Cu::init_body(const Field *field) {
  // the initial one chunk built.
  auto chunk = std::make_unique<Chunk>(this, const_cast<Field *>(field), m_cu_key);
  if (!chunk) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
    return;
  }
  m_chunks.emplace_back(std::move(chunk));

  m_current_chunk.store(0);
}

uchar *Cu::get_vfield_value(uchar *&data, size_t &len, bool need_pack) {
  ut_a(len != UNIV_SQL_NULL);

  uint32 dict_val{0u};
  switch (m_header->m_type) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      if (m_header->m_source_fld->real_type() == MYSQL_TYPE_ENUM) {
        return data;
      }
      if (need_pack) {
        auto to = std::make_unique<uchar[]>(m_header->m_source_fld->pack_length());
        auto to_ptr = Utils::Util::pack_str(data, len, &my_charset_bin, to.get(), m_header->m_source_fld->pack_length(),
                                            m_header->m_source_fld->charset());
        dict_val =
            m_header->m_local_dict->store(to_ptr, m_header->m_source_fld->pack_length(), m_header->m_encoding_type);
        *reinterpret_cast<uint32 *>(data) = dict_val;
        len = sizeof(uint32);
      } else {
        dict_val = m_header->m_local_dict->store(data, len, m_header->m_encoding_type);
        *reinterpret_cast<uint32 *>(data) = dict_val;
        len = sizeof(uint32);
      }
    } break;
    default:
      break;
  }

  return data;
}

void Cu::update_meta_info(OPER_TYPE type, uchar *data, uchar *old, bool row_reserved) {
  // gets the data value.
  // double data_val =
  //    data ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get()) : 0;
  // double old_val =
  //    old ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, old, m_header->m_local_dict.get()) : 0;

  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   *  become complex than before.*/
  switch (type) {
    case ShannonBase::OPER_TYPE::OPER_INSERT: {
      if (!data) return;  // null, only update row counts.

      double data_val =
          Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get());

      m_header->m_sum.fetch_add(data_val, std::memory_order_relaxed);

      update_min_atomic(data_val);
      update_max_atomic(data_val);

      m_stats_dirty.store(true, std::memory_order_relaxed);
    } break;

    case ShannonBase::OPER_TYPE::OPER_DELETE: {
      if (!data) return;  // value is null.

      double data_val =
          Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get());

      m_header->m_sum.fetch_sub(data_val, std::memory_order_relaxed);

      // Note: The min/max update for deletion operations is complex and requires rescanning the data
      // Here, it is simply marked as needing recalculation
      m_stats_dirty.store(true, std::memory_order_relaxed);

    } break;

    case ShannonBase::OPER_TYPE::OPER_UPDATE: {
      if (!old && !data) return;

      double old_val =
          old ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, old, m_header->m_local_dict.get()) : 0;
      double data_val =
          data ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get()) : 0;

      m_header->m_sum.fetch_sub(old_val, std::memory_order_relaxed);
      m_header->m_sum.fetch_add(data_val, std::memory_order_relaxed);

      if (data) {
        update_min_atomic(data_val);
        update_max_atomic(data_val);
      }

      m_stats_dirty.store(true, std::memory_order_relaxed);
    } break;

    default:
      break;
  }
  return;
}

void Cu::recalculate_statistics() {
  std::lock_guard<std::mutex> lock(m_stats_mutex);

  size_t total_rows = m_header->m_owner->rows(nullptr);

  if (total_rows == 0) {
    m_header->m_avg.store(0, std::memory_order_relaxed);
    m_header->m_middle.store(0, std::memory_order_relaxed);
    m_header->m_median.store(0, std::memory_order_relaxed);
    m_header->m_min.store(SHANNON_MAX_DOUBLE, std::memory_order_relaxed);
    m_header->m_max.store(SHANNON_MIN_DOUBLE, std::memory_order_relaxed);
  } else {
    // re-calc avg
    double sum = m_header->m_sum.load(std::memory_order_relaxed);
    m_header->m_avg.store(sum / total_rows, std::memory_order_relaxed);

    // calc medium（simple impl：middle of min, max）
    double min_val = m_header->m_min.load(std::memory_order_relaxed);
    double max_val = m_header->m_max.load(std::memory_order_relaxed);
    double middle = (min_val + max_val) / 2.0;

    m_header->m_middle.store(middle, std::memory_order_relaxed);
    m_header->m_median.store(middle, std::memory_order_relaxed);
  }

  m_stats_dirty.store(false, std::memory_order_relaxed);
}

void Cu::ensure_new_chunks(size_t required_chunk_id) {
  std::unique_lock lk(m_mutex);
  while (required_chunk_id >= m_chunks.size()) {
    auto chunk = std::make_unique<Chunk>(this, m_header->m_source_fld);
    if (!chunk) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
      throw std::runtime_error("Chunk allocation failed");
    }
    m_chunks.emplace_back(std::move(chunk));
  }
}

uchar *Cu::write_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  auto dlen = (len == UNIV_SQL_NULL) ? sizeof(uint32) : ((len < sizeof(uint32)) ? sizeof(uint32) : len);
  uchar local_buff[MAX_FIELD_WIDTH];
  std::unique_ptr<uchar[]> datum(dlen < MAX_FIELD_WIDTH ? nullptr : new uchar[dlen]);
  uchar *pdatum{nullptr};
  if (data) {  // not null.
    ut_a(len != UNIV_SQL_NULL);
    pdatum = (dlen < MAX_FIELD_WIDTH) ? local_buff : datum.get();
    std::memset(pdatum, 0x0, (dlen < MAX_FIELD_WIDTH) ? MAX_FIELD_WIDTH : dlen);
    std::memcpy(pdatum, data, len);
  }

  /** if the field type is text type then the value will be encoded by local dictionary.
   * otherwise, the values stores in plain format.*/
  uchar *written_to{nullptr};
  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset = rowid % SHANNON_ROWS_IN_CHUNK;

  ensure_new_chunks(chunk_id);

  auto chunk_ptr = m_chunks[chunk_id].get();
  ut_a(chunk_ptr);

  auto wlen{len};
  auto wdata = (wlen == UNIV_SQL_NULL) ? nullptr : get_vfield_value(pdatum, wlen, false);
  if (!(written_to = chunk_ptr->write(context, offset, wdata, wlen))) return nullptr;

  update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, written_to, written_to, true);
  return written_to;
}

// delete the row by rowid.
uchar *Cu::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);

  uchar *del_from{nullptr};
  if (rowid >= m_header->m_owner->rows(context))  // out of row range.
    return del_from;

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  if (chunk_id > m_chunks.size()) return del_from;  // out of chunk rnage.

  std::unique_lock lk(m_mutex);
  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  if (!(del_from = m_chunks[chunk_id]->remove(context, offset_in_chunk))) {  // ret to deleted row addr.
    return del_from;
  }

  auto is_null = Utils::Util::bit_array_get(m_chunks[chunk_id].get()->header()->m_null_mask.get(), rowid);
  update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, is_null ? nullptr : del_from, is_null ? nullptr : del_from);
  return del_from;
  // to update meta data info.
}

uchar *Cu::delete_row_all(const Rapid_load_context *context) {
  auto rows = m_header->m_owner->rows(nullptr);
  for (row_id_t rowid = 0; rowid < rows; rowid++) {
    if (!delete_row(context, rowid))  // errors occur.
      return nullptr;
  }

  // to reset all meta info. just mark. GC will do the real space reclaim.
  m_header->m_sum.store(0);
  m_header->m_avg.store(0);
  m_header->m_middle.store(0);
  m_header->m_median.store(0);
  m_header->m_max.store(SHANNON_MAX_DOUBLE);
  m_header->m_min.store(SHANNON_MIN_DOUBLE);

  ut_a(m_chunks.size());
  return m_chunks[0]->base();
}

uchar *Cu::read_row(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  uchar *ret{nullptr};
  while (m_current_chunk < m_chunks.size()) {
    if (!(ret = m_chunks[m_current_chunk].get()->read(context, data, len))) {
      // at then end of chunk, then to next
      m_current_chunk.fetch_add(1);
    } else
      break;
  }

  return ret;
}

uchar *Cu::update_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a(context);
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  auto dlen = (len < sizeof(uint32)) ? sizeof(uint32) : len;
  uchar local_buff[MAX_FIELD_WIDTH];
  std::unique_ptr<uchar[]> datum(dlen < MAX_FIELD_WIDTH ? nullptr : new uchar[dlen]);
  uchar *pdatum{nullptr};
  if (data) {
    pdatum = (dlen < MAX_FIELD_WIDTH) ? local_buff : datum.get();
    std::memset(pdatum, 0x0, (dlen < MAX_FIELD_WIDTH) ? MAX_FIELD_WIDTH : dlen);
    std::memcpy(pdatum, data, len);
  }
  auto wdata = (len == UNIV_SQL_NULL) ? nullptr : get_vfield_value(pdatum, len, false);

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  ut_a(chunk_id < m_chunks.size());

  auto ret = m_chunks[chunk_id]->update(context, offset_in_chunk, wdata, len);
  if (!ret) {
    auto old = m_chunks[chunk_id].get()->seek((row_id_t)offset_in_chunk);
    update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, data, old);
  }

  return ret;
}

uchar *Cu::update_row_from_log(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a(context);
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  auto dlen = (len < sizeof(uint32)) ? sizeof(uint32) : len;
  uchar local_buff[MAX_FIELD_WIDTH];
  std::unique_ptr<uchar[]> datum(dlen < MAX_FIELD_WIDTH ? nullptr : new uchar[dlen]);
  uchar *pdatum{nullptr};
  if (data) {
    pdatum = (dlen < MAX_FIELD_WIDTH) ? local_buff : datum.get();
    std::memset(pdatum, 0x0, (dlen < MAX_FIELD_WIDTH) ? MAX_FIELD_WIDTH : dlen);
    std::memcpy(pdatum, data, len);
  }
  auto wdata = (len == UNIV_SQL_NULL) ? nullptr : get_vfield_value(pdatum, len, false);

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  ut_a(chunk_id < m_chunks.size());

  auto ret = m_chunks[chunk_id]->update(context, offset_in_chunk, wdata, len);
  if (!ret) {
    auto old = m_chunks[chunk_id].get()->seek((row_id_t)offset_in_chunk);
    update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, data, old);
  }

  return ret;
}

ColumnStatistics::BasicStats::BasicStats()
    : min_value(DBL_MAX),
      max_value(DBL_MIN),
      sum(0.0),
      avg(0.0),
      variance(0.0),
      stddev(0.0),
      row_count(0),
      null_count(0),
      distinct_count(0),
      null_fraction(0.0),
      cardinality(0.0) {}

ColumnStatistics::StringStats::StringStats() : avg_length(0.0), max_length(0), min_length(UINT64_MAX), empty_count(0) {}

ColumnStatistics::EquiHeightHistogram::Bucket::Bucket()
    : lower_bound(0.0), upper_bound(0.0), count(0), distinct_count(0) {}

ColumnStatistics::EquiHeightHistogram::EquiHeightHistogram(size_t num_buckets) : m_bucket_count(num_buckets) {
  m_buckets.reserve(num_buckets);
}

void ColumnStatistics::EquiHeightHistogram::build(const std::vector<double> &values) {
  if (values.empty()) return;

  // 1. Sort
  std::vector<double> sorted_values = values;
  std::sort(sorted_values.begin(), sorted_values.end());

  // 2. Calculate bucket size
  size_t total_rows = sorted_values.size();
  size_t rows_per_bucket = std::max(size_t(1), total_rows / m_bucket_count);

  // 3. Allocate buckets
  m_buckets.clear();

  for (size_t i = 0; i < total_rows; i += rows_per_bucket) {
    Bucket bucket;

    size_t end = std::min(i + rows_per_bucket, total_rows);

    bucket.lower_bound = sorted_values[i];
    bucket.upper_bound = sorted_values[end - 1];
    bucket.count = end - i;

    // Calculate distinct value count
    std::unordered_set<double> distinct;
    for (size_t j = i; j < end; j++) {
      distinct.insert(sorted_values[j]);
    }
    bucket.distinct_count = distinct.size();

    m_buckets.push_back(bucket);
  }
}

double ColumnStatistics::EquiHeightHistogram::estimate_selectivity(double lower, double upper) const {
  if (m_buckets.empty()) return 0.5;

  uint64_t estimated_rows = 0;
  uint64_t total_rows = 0;

  for (const auto &bucket : m_buckets) {
    total_rows += bucket.count;

    // Calculate overlap between bucket and query range
    if (bucket.upper_bound < lower || bucket.lower_bound > upper) {
      // No overlap
      continue;
    }

    if (bucket.lower_bound >= lower && bucket.upper_bound <= upper) {
      // Fully contained
      estimated_rows += bucket.count;
    } else {
      // Partial overlap
      double bucket_range = bucket.upper_bound - bucket.lower_bound;
      double overlap_range = std::min(bucket.upper_bound, upper) - std::max(bucket.lower_bound, lower);

      if (bucket_range > 0) {
        double ratio = overlap_range / bucket_range;
        estimated_rows += static_cast<uint64_t>(bucket.count * ratio);
      }
    }
  }

  if (total_rows == 0) return 0.0;
  return static_cast<double>(estimated_rows) / total_rows;
}

double ColumnStatistics::EquiHeightHistogram::estimate_equality_selectivity(double value) const {
  for (const auto &bucket : m_buckets) {
    if (value >= bucket.lower_bound && value <= bucket.upper_bound) {
      if (bucket.distinct_count > 0) {
        return 1.0 / bucket.distinct_count * (static_cast<double>(bucket.count) / get_total_rows());
      }
    }
  }
  return 0.0;
}

uint64_t ColumnStatistics::EquiHeightHistogram::get_total_rows() const {
  uint64_t total = 0;
  for (const auto &bucket : m_buckets) {
    total += bucket.count;
  }
  return total;
}

size_t ColumnStatistics::EquiHeightHistogram::get_bucket_count() const { return m_buckets.size(); }

const std::vector<ColumnStatistics::EquiHeightHistogram::Bucket> &ColumnStatistics::EquiHeightHistogram::get_buckets()
    const {
  return m_buckets;
}

ColumnStatistics::Quantiles::Quantiles() { std::fill(std::begin(values), std::end(values), 0.0); }

void ColumnStatistics::Quantiles::compute(const std::vector<double> &sorted_values) {
  if (sorted_values.empty()) return;

  size_t n = sorted_values.size();

  for (size_t i = 0; i <= NUM_QUANTILES; i++) {
    double percentile = static_cast<double>(i) / NUM_QUANTILES;
    size_t idx = static_cast<size_t>(percentile * (n - 1));
    values[i] = sorted_values[idx];
  }
}

double ColumnStatistics::Quantiles::get_percentile(double p) const {
  if (p < 0.0) p = 0.0;
  if (p > 1.0) p = 1.0;

  size_t idx = static_cast<size_t>(p * NUM_QUANTILES);
  return values[idx];
}

double ColumnStatistics::Quantiles::get_median() const { return values[50]; }

ColumnStatistics::HyperLogLog::HyperLogLog() : m_registers(NUM_REGISTERS, 0) {}

void ColumnStatistics::HyperLogLog::add(uint64_t hash) {
  // 1. Extract register index (low REGISTER_BITS bits)
  size_t idx = hash & ((1 << REGISTER_BITS) - 1);

  // 2. Calculate leading zero count
  uint64_t remaining = hash >> REGISTER_BITS;
  uint8_t leading_zeros = count_leading_zeros(remaining) + 1;

  // 3. Update register (take maximum)
  if (leading_zeros > m_registers[idx]) {
    m_registers[idx] = leading_zeros;
  }
}

uint64_t ColumnStatistics::HyperLogLog::estimate() const {
  double alpha = 0.7213 / (1.0 + 1.079 / NUM_REGISTERS);

  double sum = 0.0;
  int zero_count = 0;

  for (size_t i = 0; i < NUM_REGISTERS; i++) {
    sum += 1.0 / (1ULL << m_registers[i]);
    if (m_registers[i] == 0) {
      zero_count++;
    }
  }

  double estimate = alpha * NUM_REGISTERS * NUM_REGISTERS / sum;

  // Small range correction
  if (estimate <= 2.5 * NUM_REGISTERS && zero_count > 0) {
    estimate = NUM_REGISTERS * std::log(static_cast<double>(NUM_REGISTERS) / zero_count);
  }

  return static_cast<uint64_t>(estimate);
}

void ColumnStatistics::HyperLogLog::merge(const HyperLogLog &other) {
  for (size_t i = 0; i < NUM_REGISTERS; i++) {
    m_registers[i] = std::max(m_registers[i], other.m_registers[i]);
  }
}

uint8_t ColumnStatistics::HyperLogLog::count_leading_zeros(uint64_t x) {
  if (x == 0) return 64;

  uint8_t count = 0;
  while ((x & (1ULL << 63)) == 0) {
    count++;
    x <<= 1;
  }
  return count;
}

ColumnStatistics::Reservoir_Sampler::Reservoir_Sampler(size_t sample_size)
    : m_sample_size(sample_size), m_seen_count(0) {
  m_samples.reserve(sample_size);
}

void ColumnStatistics::Reservoir_Sampler::add(double value) {
  m_seen_count++;

  if (m_samples.size() < m_sample_size) {
    m_samples.push_back(value);
  } else {
    // Random replacement
    size_t idx = rand() % m_seen_count;
    if (idx < m_sample_size) {
      m_samples[idx] = value;
    }
  }
}

const std::vector<double> &ColumnStatistics::Reservoir_Sampler::get_samples() const { return m_samples; }

double ColumnStatistics::Reservoir_Sampler::get_sample_rate() const {
  if (m_seen_count == 0) return 0.0;
  return static_cast<double>(m_samples.size()) / m_seen_count;
}

ColumnStatistics::ColumnStatistics(uint32_t col_id, const std::string &col_name, enum_field_types col_type)
    : m_column_id(col_id), m_column_name(col_name), m_column_type(col_type), m_version(0) {
  m_last_update = std::chrono::system_clock::now();

  // Initialize HyperLogLog
  m_hll = std::make_unique<HyperLogLog>();

  // Initialize sampler
  m_sampler = std::make_unique<Reservoir_Sampler>();

  // String type needs string statistics
  if (is_string_type()) {
    m_string_stats = std::make_unique<StringStats>();
  }
}

void ColumnStatistics::update(double value) {
  // Update basic statistics
  m_basic_stats.row_count++;
  m_basic_stats.sum += value;
  m_basic_stats.min_value = std::min(m_basic_stats.min_value, value);
  m_basic_stats.max_value = std::max(m_basic_stats.max_value, value);

  // Add to sampler
  m_sampler->add(value);

  // Update HyperLogLog
  uint64_t hash = std::hash<double>{}(value);
  m_hll->add(hash);
}

void ColumnStatistics::update(const std::string &value) {
  m_basic_stats.row_count++;

  if (value.empty()) {
    if (m_string_stats) {
      m_string_stats->empty_count++;
    }
  }

  if (m_string_stats) {
    if (value < m_string_stats->min_string || m_string_stats->min_string.empty()) {
      m_string_stats->min_string = value;
    }
    if (value > m_string_stats->max_string) {
      m_string_stats->max_string = value;
    }

    size_t len = value.length();
    m_string_stats->avg_length =
        (m_string_stats->avg_length * (m_basic_stats.row_count - 1) + len) / m_basic_stats.row_count;
    m_string_stats->max_length = std::max(m_string_stats->max_length, len);
    m_string_stats->min_length = std::min(m_string_stats->min_length, len);
  }

  // Update HyperLogLog
  uint64_t hash = std::hash<std::string>{}(value);
  m_hll->add(hash);
}

void ColumnStatistics::update_null() {
  m_basic_stats.row_count++;
  m_basic_stats.null_count++;
}

void ColumnStatistics::finalize() {
  // Calculate average
  if (m_basic_stats.row_count > 0) {
    m_basic_stats.avg = m_basic_stats.sum / m_basic_stats.row_count;
    m_basic_stats.null_fraction = static_cast<double>(m_basic_stats.null_count) / m_basic_stats.row_count;
  }

  // Estimate unique value count
  m_basic_stats.distinct_count = m_hll->estimate();

  if (m_basic_stats.row_count > 0) {
    m_basic_stats.cardinality = static_cast<double>(m_basic_stats.distinct_count) / m_basic_stats.row_count;
  }

  // Calculate variance and standard deviation
  compute_variance();

  // Build histogram
  build_histogram();

  // Calculate quantiles
  compute_quantiles();

  // Update timestamp and version
  m_last_update = std::chrono::system_clock::now();
  m_version++;
}

const ColumnStatistics::BasicStats &ColumnStatistics::get_basic_stats() const { return m_basic_stats; }

const ColumnStatistics::StringStats *ColumnStatistics::get_string_stats() const { return m_string_stats.get(); }

const ColumnStatistics::EquiHeightHistogram *ColumnStatistics::get_histogram() const { return m_histogram.get(); }

const ColumnStatistics::Quantiles *ColumnStatistics::get_quantiles() const { return m_quantiles.get(); }

double ColumnStatistics::estimate_range_selectivity(double lower, double upper) const {
  if (m_histogram) {
    return m_histogram->estimate_selectivity(lower, upper);
  }

  // Fallback: assume uniform distribution
  if (m_basic_stats.max_value == m_basic_stats.min_value) {
    return 0.0;
  }

  double range = m_basic_stats.max_value - m_basic_stats.min_value;
  double query_range = upper - lower;

  return std::min(1.0, query_range / range);
}

double ColumnStatistics::estimate_equality_selectivity(double value) const {
  if (m_histogram) {
    return m_histogram->estimate_equality_selectivity(value);
  }

  // Fallback: assume uniform distribution
  if (m_basic_stats.distinct_count > 0) {
    return 1.0 / m_basic_stats.distinct_count;
  }

  return 0.1;  // Default 10%
}

double ColumnStatistics::estimate_null_selectivity() const { return m_basic_stats.null_fraction; }

bool ColumnStatistics::serialize(std::ostream &out) const {
  // Serialize column metadata
  out.write(reinterpret_cast<const char *>(&m_column_id), sizeof(m_column_id));

  size_t name_len = m_column_name.length();
  out.write(reinterpret_cast<const char *>(&name_len), sizeof(name_len));
  out.write(m_column_name.c_str(), name_len);

  out.write(reinterpret_cast<const char *>(&m_column_type), sizeof(m_column_type));

  // Serialize basic statistics
  out.write(reinterpret_cast<const char *>(&m_basic_stats), sizeof(m_basic_stats));

  // Serialize string statistics
  bool has_string_stats = (m_string_stats != nullptr);
  out.write(reinterpret_cast<const char *>(&has_string_stats), sizeof(has_string_stats));

  if (has_string_stats) {
    // TODO: Serialize string statistics
  }

  // TODO: Serialize histogram, quantiles, etc.

  return out.good();
}

bool ColumnStatistics::deserialize(std::istream &in) {
  // TODO: Implement deserialization
  return in.good();
}

void ColumnStatistics::dump(std::ostream &out) const {
  out << "Column Statistics for '" << m_column_name << "' (ID: " << m_column_id << "):\n";
  out << "  Type: " << static_cast<int>(m_column_type) << "\n";
  out << "  Row Count: " << m_basic_stats.row_count << "\n";
  out << "  NULL Count: " << m_basic_stats.null_count << " (" << (m_basic_stats.null_fraction * 100) << "%)\n";
  out << "  Distinct Count: " << m_basic_stats.distinct_count << " (Cardinality: " << m_basic_stats.cardinality
      << ")\n";
  out << "  Min: " << m_basic_stats.min_value << "\n";
  out << "  Max: " << m_basic_stats.max_value << "\n";
  out << "  Avg: " << m_basic_stats.avg << "\n";
  out << "  StdDev: " << m_basic_stats.stddev << "\n";

  if (m_string_stats) {
    out << "  String Stats:\n";
    out << "    Min String: " << m_string_stats->min_string << "\n";
    out << "    Max String: " << m_string_stats->max_string << "\n";
    out << "    Avg Length: " << m_string_stats->avg_length << "\n";
    out << "    Empty Count: " << m_string_stats->empty_count << "\n";
  }

  if (m_histogram) {
    out << "  Histogram: " << m_histogram->get_bucket_count() << " buckets\n";
  }
}

void ColumnStatistics::compute_variance() {
  const auto &samples = m_sampler->get_samples();
  if (samples.size() < 2) return;

  double mean = m_basic_stats.avg;
  double sum_sq_diff = 0.0;

  for (double value : samples) {
    double diff = value - mean;
    sum_sq_diff += diff * diff;
  }

  m_basic_stats.variance = sum_sq_diff / (samples.size() - 1);
  m_basic_stats.stddev = std::sqrt(m_basic_stats.variance);
}

void ColumnStatistics::build_histogram() {
  const auto &samples = m_sampler->get_samples();
  if (samples.size() < 100) return;  // Too few samples

  m_histogram = std::make_unique<EquiHeightHistogram>(64);
  m_histogram->build(samples);
}

void ColumnStatistics::compute_quantiles() {
  auto samples = m_sampler->get_samples();
  if (samples.size() < 100) return;

  std::sort(samples.begin(), samples.end());

  m_quantiles = std::make_unique<Quantiles>();
  m_quantiles->compute(samples);
}

bool ColumnStatistics::is_string_type() const {
  return m_column_type == MYSQL_TYPE_VARCHAR || m_column_type == MYSQL_TYPE_STRING ||
         m_column_type == MYSQL_TYPE_VAR_STRING || m_column_type == MYSQL_TYPE_BLOB;
}

CU::CU(Imcu *owner, const FieldMetadata &field_meta, uint32_t col_idx, size_t capacity,
       std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool) {
  m_header.owner_imcu = owner;
  m_header.column_id = col_idx;
  m_header.field_metadata = field_meta.source_fld;
  m_header.type = field_meta.type;
  m_header.pack_length = field_meta.pack_length;
  m_header.normalized_length = field_meta.normalized_length;
  m_header.charset = field_meta.charset;
  m_header.capacity = capacity;
  m_header.local_dict = field_meta.dictionary;

  m_data_capacity = capacity * m_header.normalized_length;
  uchar *raw_ptr = static_cast<uchar *>(mem_pool.get()->allocate_auto(m_data_capacity, shannon_data_arear));
  if (!raw_ptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "CU memory allocation failed");
    return;
  }

  m_data = std::unique_ptr<uchar[], PoolDeleter>(raw_ptr, PoolDeleter(mem_pool.get(), m_data_capacity));

  m_version_manager = std::make_unique<Column_Version_Manager>();
}

CU::~CU() {}

void CU::Column_Version_Manager::create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn,
                                                const uchar *old_value, size_t len) {
  std::unique_lock lock(m_mutex);

  // create a new version.
  auto new_version = std::make_unique<Column_Version>();
  new_version->txn_id = txn_id;
  new_version->scn = scn;
  new_version->timestamp = std::chrono::system_clock::now();
  new_version->value_length = len;

  // copy the old value.
  if (len != UNIV_SQL_NULL && old_value) {
    new_version->old_value = std::make_unique<uchar[]>(len);
    std::memcpy(new_version->old_value.get(), old_value, len);
  }

  // add to version link.
  auto it = m_versions.find(local_row_id);
  if (it != m_versions.end()) {
    new_version->prev = it->second.release();
    it->second = std::move(new_version);
  } else {
    m_versions[local_row_id] = std::move(new_version);
  }
}

bool CU::Column_Version_Manager::get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer,
                                                  size_t &len) const {
  return false;
}

size_t CU::Column_Version_Manager::purge(uint64_t min_active_scn) {
  size_t purged = 0;
  std::unique_lock lock(m_mutex);
  for (auto it = m_versions.begin(); it != m_versions.end();) {
    Column_Version_Manager::Column_Version *head = it->second.get();
    Column_Version_Manager::Column_Version *current = head;
    Column_Version_Manager::Column_Version *prev_valid = nullptr;

    bool found_visible = false;

    while (current != nullptr) {
      // Earlier than the minimum active SCN and not the latest visible version
      if (current->scn < min_active_scn && found_visible) {
        // purgeable.
        Column_Version_Manager::Column_Version *to_delete = current;
        current = current->prev;

        if (prev_valid) {
          prev_valid->prev = current;
        }

        delete to_delete;
        purged++;
      } else {
        // keep.
        found_visible = true;
        prev_valid = current;
        current = current->prev;
      }
    }

    // If the entire version chain has been purged
    if (head == nullptr) {
      it = m_versions.erase(it);
    } else {
      ++it;
    }
  }

  return purged;
}

/*
 * write a new value (Internal deletion/NULL management is handled by IMCU)
 */
bool CU::write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len) {
  if (local_row_id >= m_header.capacity) return false;

  std::lock_guard lock(m_data_mutex);
  uchar *dest = m_data.get() + local_row_id * m_header.normalized_length;

  if (data == nullptr) {  // NULL values: The IMCU's null_mask has been set, write placeholder values here
    std::memset(dest, 0, m_header.normalized_length);
    m_header.null_count.fetch_add(1);
    m_header.data_size.fetch_add(m_header.normalized_length);
  } else {
    if (m_header.local_dict) {
      uint32_t dict_id = m_header.local_dict->store(data, len, m_header.encoding);
      std::memcpy(dest, &dict_id, sizeof(uint32_t));
      m_header.data_size.fetch_add(sizeof(uint32_t));
    } else {
      std::memcpy(dest, data, std::min(len, m_header.normalized_length.load()));
      m_header.data_size.fetch_add(std::min(len, m_header.normalized_length.load()));
    }
    update_statistics(data, len);
  }

  m_header.total_count.fetch_add(1);
  return false;
}

int CU::update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len) {
  // 1. read old value;
  uchar old_value[MAX_FIELD_WIDTH] = {0};
  size_t old_len = read(context, local_row_id, old_value);

  // 2. Create column-level version (only store old values of this column)
  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;

  m_version_manager->create_version(local_row_id, txn_id, scn, old_value, old_len);

  // 3. write new value.
  {
    std::lock_guard lock(m_data_mutex);

    auto dest = static_cast<void *>(const_cast<uchar *>(get_data_address(local_row_id)));

    if (len == UNIV_SQL_NULL) {
      // NULL values are handled by the null_mask of IMCU
      std::memset(dest, 0x0, m_header.normalized_length);
    } else {
      // dealing with encoding.
      if (m_header.local_dict) {
        uint32_t dict_id = m_header.local_dict.get()->store(new_data, len, m_header.encoding);
        std::memcpy(dest, &dict_id, sizeof(uint32_t));
      } else {
        std::memcpy(dest, new_data, std::min(len, m_header.normalized_length.load(std::memory_order_relaxed)));
      }
    }
  }

  // 4. update statistics.
  update_statistics(new_data, len);

  return ShannonBase::SHANNON_SUCCESS;
}

bool CU::write_batch(const Rapid_context *context, row_id_t start_row, const std::vector<uchar *> &data_array,
                     size_t count) {
  return false;
}

size_t CU::read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const {
  if (local_row_id >= m_header.capacity) return 0;

  // check IMCU's NULL mask.
  if (m_header.owner_imcu->is_null(m_header.column_id, local_row_id)) {
    return UNIV_SQL_NULL;
  }

  const uchar *src = m_data.get() + local_row_id * m_header.normalized_length;

  // dealing with dictionary encode.
  if (m_header.local_dict) {
    uint32_t dict_id = *reinterpret_cast<const uint32_t *>(src);
    auto decode_str = m_header.local_dict->get(dict_id);
    std::memcpy(buffer, decode_str.c_str(), decode_str.length());
    return decode_str.length();
  }

  std::memcpy(buffer, src, m_header.normalized_length);
  return m_header.normalized_length;
}

size_t CU::read(const Rapid_context *context, row_id_t local_row_id, uint64_t target_scn, uchar *buffer) const {
  return 0;
}

const uchar *read(const Rapid_context *context, row_id_t local_row_id) { return nullptr; }

size_t CU::read_batch(const Rapid_context *context, const std::vector<row_id_t> &row_ids, uchar *output) const {
  return 0;
}

size_t CU::scan_range(const Rapid_context *context, row_id_t start_row, size_t count, uchar *output) const { return 0; }

void CU::create_version(const Rapid_context *context, row_id_t local_row_id) {}

size_t CU::purge_versions(const Rapid_context *context, uint64_t min_active_scn) {
  return m_version_manager->purge(min_active_scn);
}

bool CU::encode_value(const Rapid_context *context, const uchar *data, size_t len, uint32_t &encoded) { return false; }

bool CU::decode_value(const Rapid_context *context, uint32_t encoded, uchar *buffer, size_t &len) const {
  return false;
}

bool CU::compress() { return false; }

bool CU::decompress() { return false; }

void CU::update_statistics(const uchar *data, size_t len) {
  if (!is_integer_type(m_header.type)) return;

  double value = Utils::Util::get_field_numeric<double>(m_header.field_metadata, data, nullptr);
  m_header.sum.fetch_add(value);

  m_header.min_value.store(std::min(m_header.min_value.load(std::memory_order_relaxed), value));
  m_header.max_value.store(std::max(m_header.max_value.load(std::memory_order_relaxed), value));

  m_header.avg = m_header.sum.load(std::memory_order_relaxed) / m_header.total_count.load(std::memory_order_relaxed);
}

bool CU::serialize(std::ostream &out) const { return false; }

bool CU::deserialize(std::istream &in) { return false; }

double CU::get_numeric_value(const Rapid_context *context, const uchar *data, size_t len) const { return 0.0f; }

}  // namespace Imcs
}  // namespace ShannonBase