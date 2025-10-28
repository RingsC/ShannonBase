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

   The fundmental code for imcs. Rapid Table.
*/

/**DataTable to mock a table hehaviors. We can use a DataTable to open the IMCS
 * with sepecific table information. After the Cu belongs to this table were found
 * , we can use this DataTable object to read/write, etc., just like a normal innodb
 * table.
 */
#include "storage/rapid_engine/imcs/table.h"

#include <regex>
#include <sstream>

#include "include/ut0dbg.h"  //ut_a
#include "sql/field.h"       //field
#include "sql/table.h"       //TABLE
#include "storage/innobase/include/mach0data.h"

#include "storage/rapid_engine/imcs/chunk.h"           //CHUNK
#include "storage/rapid_engine/imcs/cu.h"              //CU
#include "storage/rapid_engine/include/rapid_const.h"  // INVALID_ROW_ID

#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/predicate.h"  //predicate
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/utils/utils.h"  //Blob
namespace ShannonBase {
namespace Imcs {

/**
 * @brief Encode a row buffer into a contiguous key buffer suitable for indexing.
 *
 * This function constructs a binary key representation from a MySQL row record
 * according to the specified KEY metadata. It handles null flags, variable-length
 * fields, BLOBs, and numeric types, ensuring proper encoding for index comparison.
 *
 * @param[out] to_key      Pointer to pre-allocated key buffer to write encoded key.
 * @param[in]  from_record Pointer to row data buffer containing raw field values.
 * @param[in]  key_info    Pointer to MySQL KEY structure describing key parts.
 * @param[in]  key_len     Total length of the key buffer.
 *
 * @note
 *   - Handles null indicators for columns that have a null bit.
 *   - Numeric types (DOUBLE, FLOAT, DECIMAL, NEWDECIMAL, LONG) are encoded
 *     in sortable binary format using Index::Encoder.
 *   - Fixed-length, variable-length, and BLOB columns are encoded according
 *     to MySQL key conventions (HA_KEY_BLOB_LENGTH for BLOBs).
 *   - The function does not modify the input record.
 */
static void encode_row_key(uchar *to_key, const uchar *from_record, const KEY *key_info, uint key_len) {
  if (!to_key || !from_record || !key_info || key_len == 0) return;

  memset(to_key, 0x0, key_len);

  uint length{0u};
  auto remain_len = key_len;

  for (KEY_PART_INFO *key_part = key_info->key_part; remain_len > 0; key_part++) {
    if (key_part->null_bit) {
      bool is_null = from_record[key_part->null_offset] & key_part->null_bit;
      *to_key++ = (is_null ? 1 : 0);
      remain_len--;
    }

    length = std::min<uint>(remain_len, key_part->length);
    Field *field = key_part->field;
    const CHARSET_INFO *cs = field->charset();

    if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
      remain_len -= HA_KEY_BLOB_LENGTH;
      length = std::min<uint>(remain_len, key_part->length);
      field->get_key_image(to_key, length, Field::itRAW);
      to_key += HA_KEY_BLOB_LENGTH;
    } else {
      switch (field->type()) {
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
          uchar encoding[8] = {0};
          Index::Encoder<double>::EncodeData(field->val_real(), encoding);
          memcpy(to_key, encoding, length);  // decimal stored length: 5 not 8.
        } break;
        case MYSQL_TYPE_LONG: {
          ut_a(length == sizeof(int32_t));
          uchar encoding[4] = {0};
          Index::Encoder<int32_t>::EncodeData((int32_t)field->val_int(), encoding);
          memcpy(to_key, encoding, length);
        } break;
        default: {
          ut_a(length == field->pack_length());
          size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
          if (bytes < length) {
            cs->cset->fill(cs, reinterpret_cast<char *>(to_key + bytes), length - bytes, ' ');
          }
        } break;
      }
    }

    to_key += length;
    remain_len -= length;
  }
}

/**
 * @brief Encode a key directly from a row using column offsets and null bitmaps.
 *
 * This function converts a row buffer into a binary key representation, taking
 * into account the column offsets, null-byte positions, and null-bit masks.
 * The encoded key is written into a shared key buffer. A shared mutex is used
 * to ensure thread-safe access to the key buffer.
 *
 * @param[in]  rowdata          Pointer to the raw row buffer.
 * @param[in]  col_offsets      Array of column byte offsets within the row.
 * @param[in]  null_byte_offsets Array of byte offsets where null bits reside.
 * @param[in]  null_bitmasks    Array of bitmasks for testing null flags.
 * @param[in]  key              Pointer to KEY metadata describing the key.
 * @param[out] key_buff         Pointer to pre-allocated buffer to store encoded key.
 * @param[in,out] key_buff_mutex Shared mutex to protect concurrent writes to key_buff.
 *
 * @note
 *   - Encodes null flags into the first byte of each key part if applicable.
 *   - Numeric types are encoded via Index::Encoder to preserve sort order.
 *   - BLOB and variable-length parts are encoded using MySQL HA_KEY_BLOB_LENGTH.
 *   - The key buffer is locked during the encoding process to ensure thread safety.
 *   - This function may modify the field pointers of `Field` objects temporarily
 *     to point into the row buffer for key extraction.
 */
static void encode_key_from_row(const uchar *rowdata, const ulong *col_offsets, const ulong *null_byte_offsets,
                                const ulong *null_bitmasks, const KEY *key, uchar *key_buff,
                                std::shared_mutex &key_buff_mutex) {
  if (!rowdata || !key) return;

  auto to_key = key_buff;

  uint length{0u};
  KEY_PART_INFO *key_part;
  auto key_length = key->key_length;
  { /* Copy the key parts */
    std::unique_lock<std::shared_mutex> ex_lk(key_buff_mutex);
    for (key_part = key->key_part; (int)key_length > 0; key_part++) {
      Field *field = key_part->field;
      const CHARSET_INFO *cs = field->charset();
      auto fld_ptr = rowdata + ptrdiff_t(col_offsets[field->field_index()]);
      field->set_field_ptr(const_cast<uchar *>(fld_ptr));

      if (key_part->null_bit) {
        bool key_is_null = rowdata[key_part->null_offset] & key_part->null_bit;
        // ut_a(is_field_null(field->field_index(), rowdata, null_byte_offsets, null_bitmasks) == key_is_null);
        *to_key++ = (key_is_null ? 1 : 0);
        key_length--;
      }

      if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
        key_length -= HA_KEY_BLOB_LENGTH;
        length = std::min<uint>(key_length, key_part->length);
        field->get_key_image(to_key, length, Field::itRAW);
        to_key += HA_KEY_BLOB_LENGTH;
      } else {
        length = std::min<uint>(key_length, key_part->length);
        switch (field->type()) {
          case MYSQL_TYPE_DOUBLE:
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DECIMAL:
          case MYSQL_TYPE_NEWDECIMAL: {
            uchar encoding[8] = {0};
            Index::Encoder<double>::EncodeData(field->val_real(), encoding);  // decimal stored length: 5 not 8.
            memcpy(to_key, encoding, length);
          } break;
          case MYSQL_TYPE_LONG: {
            ut_a(length == sizeof(int32_t));
            uchar encoding[4] = {0};
            Index::Encoder<int32_t>::EncodeData((int32_t)field->val_int(), encoding);
            memcpy(to_key, encoding, length);
          } break;
          default: {
            const size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
            if (bytes < length) cs->cset->fill(cs, (char *)to_key + bytes, length - bytes, ' ');
          } break;
        }
      }
      to_key += length;
      key_length -= length;
    }
  }  // scope lock end.
}

int Table::create_fields_memo(const Rapid_load_context *context) {
  ut_a(context && context->m_table);
  auto source = context->m_table;

  for (auto index = 0u; index < source->s->fields; index++) {
    auto field = *(source->field + index);
    if (field->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    size_t chunk_size = SHANNON_ROWS_IN_CHUNK * Utils::Util::normalized_length(field);
    if (likely(ShannonBase::rapid_allocated_mem_size + chunk_size > ShannonBase::rpd_mem_sz_max)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");

      m_fields.clear();
      return HA_ERR_GENERIC;
    }

    std::unique_lock<std::shared_mutex> lk(m_fields_mutex);
    m_fields.emplace(field->field_name, std::make_unique<Cu>(this, field, field->field_name));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::create_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;
  ut_a(source);
  // no.1: primary key. using row_id as the primary key when missing user-defined pk.
  if (source->s->is_missing_primary_key()) build_hidden_index_memo(context);

  // no.2: user-defined indexes.
  build_user_defined_index_memo(context);
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::build_hidden_index_memo(const Rapid_load_context *context) {
  m_source_keys.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                        std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{SHANNON_DB_ROW_ID}));
  m_indexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                    std::make_unique<Index::Index<uchar, row_id_t>>(ShannonBase::SHANNON_PRIMARY_KEY_NAME));
  m_index_mutexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME, std::make_unique<std::mutex>());
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;

  for (auto ind = 0u; ind < source->s->keys; ind++) {
    auto key_info = source->key_info + ind;
    std::vector<std::string> key_parts_names;
    for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
      key_parts_names.push_back(key_info->key_part[i].field->field_name);
    }

    m_source_keys.emplace(key_info->name, std::make_pair(key_info->key_length, key_parts_names));
    m_indexes.emplace(key_info->name, std::make_unique<Index::Index<uchar, row_id_t>>(key_info->name));
    m_index_mutexes.emplace(key_info->name, std::make_unique<std::mutex>());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

Cu *Table::get_field(std::string field_name) {
  std::shared_lock<std::shared_mutex> lk(m_fields_mutex);
  if (m_fields.find(field_name) == m_fields.end()) return nullptr;
  return m_fields[field_name].get();
}

int Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  // ref: void key_copy(uchar *to_key, const uchar *from_record, const KEY *key_info,
  //            uint key_length). Due to we should encoding the float/double/decimal types.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();
    encode_row_key(to_key, from_record, key, key->key_length);
  }
  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[keypart].get());
    m_indexes[keypart].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                     sizeof(rowid));
  }
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

// using for parallel load.
int Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                            ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;
  std::unique_ptr<uchar[]> key_buff{nullptr};
  auto key_len{0u};
  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    // In parallel scan, the primary key must be have, otherwise sequential scan.

    ut_a(source->file->ref_length == SHANNON_DATA_DB_ROW_ID_LEN);
    key_len = source->file->ref_length;
    key_buff.reset(new uchar[key_len]);
    memset(key_buff.get(), 0x0, key_len);
    memcpy(key_buff.get(), source->file->ref, key_len);
  } else {
    /* Copy primary key as the row reference */
    key_len = key->key_length;
    key_buff.reset(new uchar[key_len]);
    memset(key_buff.get(), 0x0, key_len);
    auto to_key = key_buff.get();
    encode_key_from_row(rowdata, col_offsets, null_byte_offsets, null_bitmasks, key, to_key, m_key_buff_mutex);
  }

  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[keypart].get());
    m_indexes[keypart].get()->insert(key_buff.get(), key_len, &rowid, sizeof(rowid));
  }
  return SHANNON_SUCCESS;
}

int Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  return build_index_impl(context, key, rowid);
}

// using for parallel load.
int Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                       ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  return build_index_impl(context, key, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks);
}

int Table::build_key_info(const Rapid_load_context *context, const KEY *key) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  // ref: void key_copy(uchar *to_key, const uchar *from_record, const KEY *key_info,
  //            uint key_length). Due to we should encoding the float/double/decimal types.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();
    encode_row_key(to_key, from_record, key, key->key_length);
  }

  return SHANNON_SUCCESS;
}

int Table::build_key_info(const Rapid_load_context *context, const KEY *key, uchar *rowdata, ulong *col_offsets,
                          ulong *null_byte_offsets, ulong *null_bitmasks) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();
    encode_key_from_row(rowdata, col_offsets, null_byte_offsets, null_bitmasks, key, to_key, m_key_buff_mutex);
  }

  return SHANNON_SUCCESS;
}

int Table::write(const Rapid_load_context *context, uchar *data) {
  /**
   * for VARCHAR type Data in field->ptr is stored as: 1 or 2 bytes length-prefix-header  (from
   * Field_varstring::length_bytes) data. the here we dont use var_xxx to get data, rather getting
   * directly, due to we dont care what real it is. ref to: field.cc:6703
   */

  auto rowid = forward_rowid(context);

  Utils::ColumnMapGuard guard(context->m_table);
  for (auto index = 0u; index < context->m_table->s->fields; index++) {
    auto fld = *(context->m_table->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};
    if (fld->is_null()) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
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
    }

    if (!(m_fields[fld->field_name]->write_row(context, rowid, data_ptr, data_len))) {
      backward_rowid(context);
      return HA_ERR_GENERIC;
    }
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid)) {
      backward_rowid(context);
      return HA_ERR_GENERIC;
    }
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, rowid)) {
      backward_rowid(context);
      return HA_ERR_GENERIC;
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

// using for parallel load. change the parttable correspondingly.
int Table::write(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                 ulong *null_byte_offsets, ulong *null_bitmasks) {
  ut_a(context->m_table->s->fields == n_cols);
  uchar *data_ptr{nullptr};
  uint data_len{0};

  auto rowid = forward_rowid(context);

  Utils::ColumnMapGuard guard(context->m_table);
  for (auto col_ind = 0u; col_ind < context->m_table->s->fields; col_ind++) {
    auto fld = *(context->m_table->field + col_ind);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    data_ptr = rowdata + col_offsets[col_ind];
    auto is_null = (fld->is_nullable()) ? is_field_null(col_ind, rowdata, null_byte_offsets, null_bitmasks) : false;

    if (is_null) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
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
    }

    if (!(m_fields[fld->field_name]->write_row(context, rowid, data_ptr, data_len))) {
      // TODO: mark this row to be junk.
      backward_rowid(context);
      return HA_ERR_GENERIC;
    }
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)rowdata);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks)) {
      backward_rowid(context);
      return HA_ERR_GENERIC;
    }
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks)) {
      backward_rowid(context);
      return HA_ERR_GENERIC;
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::rollback_changes_by_trxid(Transaction::ID trxid) {
  for (auto &cu : m_fields) {
    auto chunk_sz = cu.second.get()->chunks();
    for (auto index = 0u; index < chunk_sz; index++) {
      auto &version_infos = cu.second.get()->chunk(index)->header()->m_smu->version_info();
      if (!version_infos.size()) continue;

      for (auto &ver : version_infos) {
        std::lock_guard<std::mutex> lock(ver.second.vec_mutex);
        auto rowid = ver.first;

        std::for_each(ver.second.items.begin(), ver.second.items.end(), [&](ReadView::SMU_item &item) {
          if (item.trxid == trxid) {
            // To update rows status.
            if (item.oper_type == OPER_TYPE::OPER_INSERT) {                      //
              if (!cu.second.get()->chunk(index)->header()->m_del_mask.get()) {  // the del mask not exists now.
                cu.second.get()->chunk(index)->header()->m_del_mask =
                    std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
              }
              Utils::Util::bit_array_set(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
            }
            if (item.oper_type == OPER_TYPE::OPER_DELETE) {
              Utils::Util::bit_array_reset(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
            }
            item.tm_committed = ShannonBase::SHANNON_MAX_STMP;  // reset commit timestamp to max, mean it rollbacked.
                                                                // has been rollbacked, invisible to all readview.
          }
        });
      }
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                              std::unordered_map<std::string, mysql_field_t> &fields) {
  for (auto &field_val : fields) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || m_fields.find(key_name) == m_fields.end()) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!m_fields[key_name]->write_row(context, rowid, field_val.second.data.get(), len)) {
      // TODO: mark this row to be junk.
      return HA_ERR_WRONG_IN_RECORD;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                      const uchar *new_field_data, size_t nlen) {
  if (m_fields.find(field_key) == m_fields.end()) return HA_ERR_GENERIC;

  auto ret = m_fields[field_key]->update_row(context, rowid, const_cast<uchar *>(new_field_data), nlen);
  if (!ret) return HA_ERR_GENERIC;
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                               std::unordered_map<std::string, mysql_field_t> &upd_recs) {
  for (auto &field_val : upd_recs) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || m_fields.find(key_name) == m_fields.end()) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!m_fields[key_name]->update_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::update_row(const Rapid_load_context *context, size_t nlen, ulong *col_offsets, ulong *null_byte_offsets,
                      ulong *null_bitmasks, const uchar *start, const uchar *new_start) {
  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position(start);  // to set DB_ROW_ID.
    if (build_key_info(context, nullptr, (uchar *)start, col_offsets, null_byte_offsets, null_bitmasks))
      return HA_ERR_GENERIC;
  }

  Utils::ColumnMapGuard guard(context->m_table);
  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (!strncmp(key_info->name, ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                 strlen(ShannonBase::SHANNON_PRIMARY_KEY_NAME))) {
      if (build_key_info(context, key_info, (uchar *)start, col_offsets, null_byte_offsets, null_bitmasks))
        return HA_ERR_GENERIC;
      break;
    }
  }
  auto rowid = m_indexes[ShannonBase::SHANNON_PRIMARY_KEY_NAME].get()->lookup(context->m_extra_info.m_key_buff.get(),
                                                                              context->m_extra_info.m_key_len);
  auto found_rowid = rowid ? *rowid : INVALID_ROW_ID;
  if (found_rowid == INVALID_ROW_ID) return HA_ERR_KEY_NOT_FOUND;

  uchar *data_ptr{nullptr};
  uint data_len{0};
  for (auto col_ind = 0u; col_ind < context->m_table->s->fields; col_ind++) {
    auto fld = *(context->m_table->field + col_ind);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    data_ptr = (uchar *)new_start + col_offsets[col_ind];
    auto is_null = (fld->is_nullable()) ? is_field_null(col_ind, new_start, null_byte_offsets, null_bitmasks) : false;

    if (is_null) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
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
    }

    if (!(m_fields[fld->field_name]->update_row(context, found_rowid, data_ptr, data_len))) {
      return HA_ERR_GENERIC;
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  for (auto it = m_fields.begin(); it != m_fields.end();) {
    if (!it->second->delete_row(context, rowid)) {
      return HA_ERR_GENERIC;
    }
    ++it;
  }

  return SHANNON_SUCCESS;
}

int Table::delete_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                      ulong *null_byte_offsets, ulong *null_bitmasks) {
  ut_a(context->m_table->s->fields == n_cols);

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)rowdata);  // to set DB_ROW_ID.
    if (build_key_info(context, nullptr, rowdata, col_offsets, null_byte_offsets, null_bitmasks)) return HA_ERR_GENERIC;
  }

  Utils::ColumnMapGuard guard(context->m_table);
  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (!strncmp(key_info->name, ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                 strlen(ShannonBase::SHANNON_PRIMARY_KEY_NAME))) {
      if (build_key_info(context, key_info, rowdata, col_offsets, null_byte_offsets, null_bitmasks))
        return HA_ERR_GENERIC;
      break;
    }
  }
  auto rowid = m_indexes[ShannonBase::SHANNON_PRIMARY_KEY_NAME].get()->lookup(context->m_extra_info.m_key_buff.get(),
                                                                              context->m_extra_info.m_key_len);
  auto found_rowid = rowid ? *rowid : INVALID_ROW_ID;
  if (found_rowid == INVALID_ROW_ID) return HA_ERR_KEY_NOT_FOUND;

  return delete_row(context, found_rowid);
}

int Table::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) {
  if (!m_fields.size()) return SHANNON_SUCCESS;

  if (rowids.empty()) {  // delete all rows.
    for (auto &cu : m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row_all(context)) return HA_ERR_GENERIC;
    }

    return ShannonBase::SHANNON_SUCCESS;
  }

  for (auto &rowid : rowids) {
    for (auto &cu : m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row(context, rowid)) return HA_ERR_GENERIC;
    }
  }
  // TODO: remove the index item.

  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_partitions(const Rapid_load_context *context) {
  auto ret{ShannonBase::SHANNON_SUCCESS};
  auto source = context->m_table;

  // start to add partitions.
  for (auto &[part_name, part_id] : context->m_extra_info.m_partition_infos) {
    auto part_key = part_name;
    part_key.append("#").append(std::to_string(part_id));
    auto table = std::make_unique<Table>(source->s->db.str, source->s->table_name.str, part_key);

    // step 1: build the Cus meta info for every column.
    if ((ret = table.get()->create_fields_memo(context))) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Build fields memo for partition failed");
      return ret;
    }

    // step 2: build indexes.
    if ((ret = table.get()->create_index_memo(context))) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Build indexes memo for partition failed");
      return ret;
    }

    // step 3: set load type.
    table.get()->set_load_type(RapidTable::LoadType::USER_LOADED);

    // step 4: Adding the Table meta obj into partitions table meta information.
    m_partitions.emplace(part_key, std::move(table));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

Normal_Table::Normal_Table(const TABLE *&mysql_table, const TableConfig &config) : RpdTable(mysql_table, config) {
  Utils::MemoryPool::Config mem_config;
  m_memory_pool = std::make_shared<Utils::MemoryPool>(mem_config);
  m_memory_pool.get()->set_tenant_quota(shannon_data_arear, SHANNON_DEFAULT_MEMRORY_SIZE);

  m_metadata.db_name = mysql_table->s->db.str;
  m_metadata.table_name = mysql_table->s->table_name.str;
  m_metadata.table_id = generate_table_id();
  m_metadata.rows_per_imcu = config.rows_per_imcu;
  m_metadata.max_imcu_size_mb = config.max_imcu_size_mb;

  // from MySQL TABLE get fields infor.
  m_metadata.num_columns = mysql_table->s->fields;
  m_metadata.fields.reserve(m_metadata.num_columns);

  for (uint32_t ind = 0; ind < m_metadata.num_columns; ind++) {
    Field *field = mysql_table->field[ind];

    std::string comment;
    if (field->comment.str && field->comment.length > 0) {
      comment = std::string(field->comment.str, field->comment.length);
      std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
    }

    Compress::Encoding_type encoding = Compress::Encoding_type::NONE;
    const char *const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
    std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs | std::regex_constants::icase);

    if (std::regex_search(comment, column_encoding_patt)) {
      if (comment.find("SORTED") != std::string::npos)
        encoding = Compress::Encoding_type::SORTED;
      else if (comment.find("VARLEN") != std::string::npos)
        encoding = Compress::Encoding_type::VARLEN;
    }

    m_metadata.fields.emplace_back(FieldMetadata{
        .source_fld = field->clone(m_mem_root.get()),
        .field_id = ind,
        .field_name = (field->field_name && field->field_name[0] != '\0') ? std::string(field->field_name) : "unknown",
        .type = field->type(),
        .pack_length = field->pack_length(),
        .normalized_length = Utils::Util::normalized_length(field),
        .nullable = field->is_nullable(),
        .is_key = field->is_flag_set(PRI_KEY_FLAG),
        .is_secondary_field = !field->is_flag_set(NOT_SECONDARY_FLAG),
        .encoding = encoding,
        .charset = field->charset(),
        .dictionary = is_string_type(field->type()) ? std::make_shared<Compress::Dictionary>(encoding) : nullptr,
        .global_min = 0.0,
        .global_max = 0.0,
        .distinct_count = 0,
        .null_ratio = 0.0});
  }

  // [TODO] intial global compoent.
  // m_txn_coordinator = std::make_unique<Transaction_Coordinator>();
  // m_version_manager = std::make_unique<Global_Version_Manager>();
  // m_bg_workers = std::make_unique<Background_Worker_Pool>(config.background_worker_threads);

  // create intial IMCU
  create_initial_imcu();
}

int Normal_Table::build_hidden_index_memo(const Rapid_load_context *context) {
  // m_source_keys.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
  //                       std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{SHANNON_DB_ROW_ID}));
  m_indexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                    std::make_unique<Index::Index<uchar, row_id_t>>(ShannonBase::SHANNON_PRIMARY_KEY_NAME));
  m_index_mutexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME, std::make_unique<std::mutex>());
  return ShannonBase::SHANNON_SUCCESS;
}

int Normal_Table::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;

  for (auto ind = 0u; ind < source->s->keys; ind++) {
    auto key_info = source->key_info + ind;
    std::vector<std::string> key_parts_names;
    for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
      key_parts_names.push_back(key_info->key_part[i].field->field_name);
    }

    // m_source_keys.emplace(key_info->name, std::make_pair(key_info->key_length, key_parts_names));
    m_indexes.emplace(key_info->name, std::make_unique<Index::Index<uchar, row_id_t>>(key_info->name));
    m_index_mutexes.emplace(key_info->name, std::make_unique<std::mutex>());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Normal_Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  // ref: void key_copy(uchar *to_key, const uchar *from_record, const KEY *key_info,
  //            uint key_length). Due to we should encoding the float/double/decimal types.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();
    encode_row_key(to_key, from_record, key, key->key_length);
  }
  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[keypart].get());
    m_indexes[keypart].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                     sizeof(rowid));
  }
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

// using for parallel load.
int Normal_Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                                   ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;
  std::unique_ptr<uchar[]> key_buff{nullptr};
  auto key_len{0u};
  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    // In parallel scan, the primary key must be have, otherwise sequential scan.

    ut_a(source->file->ref_length == SHANNON_DATA_DB_ROW_ID_LEN);
    key_len = source->file->ref_length;
    key_buff.reset(new uchar[key_len]);
    memset(key_buff.get(), 0x0, key_len);
    memcpy(key_buff.get(), source->file->ref, key_len);
  } else {
    /* Copy primary key as the row reference */
    key_len = key->key_length;
    key_buff.reset(new uchar[key_len]);
    memset(key_buff.get(), 0x0, key_len);
    auto to_key = key_buff.get();
    std::shared_mutex key_buff_mutex;
    encode_key_from_row(rowdata, col_offsets, null_byte_offsets, null_bitmasks, key, to_key, key_buff_mutex);
  }

  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[keypart].get());
    m_indexes[keypart].get()->insert(key_buff.get(), key_len, &rowid, sizeof(rowid));
  }
  return SHANNON_SUCCESS;
}

int Normal_Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  return build_index_impl(context, key, rowid);
}

// using for parallel load.
int Normal_Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                              ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  return build_index_impl(context, key, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks);
}

int Normal_Table::build_key_info(const Rapid_load_context *context, const KEY *key) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  // ref: void key_copy(uchar *to_key, const uchar *from_record, const KEY *key_info,
  //            uint key_length). Due to we should encoding the float/double/decimal types.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();
    encode_row_key(to_key, from_record, key, key->key_length);
  }

  return SHANNON_SUCCESS;
}

int Normal_Table::build_key_info(const Rapid_load_context *context, const KEY *key, uchar *rowdata, ulong *col_offsets,
                                 ulong *null_byte_offsets, ulong *null_bitmasks) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();
    std::shared_mutex key_buff_mutex;
    encode_key_from_row(rowdata, col_offsets, null_byte_offsets, null_bitmasks, key, to_key, key_buff_mutex);
  }

  return SHANNON_SUCCESS;
}

int Normal_Table::create_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;
  ut_a(source);
  // no.1: primary key. using row_id as the primary key when missing user-defined pk.
  if (source->s->is_missing_primary_key()) build_hidden_index_memo(context);

  // no.2: user-defined indexes.
  build_user_defined_index_memo(context);
  return ShannonBase::SHANNON_SUCCESS;
}

row_id_t Normal_Table::insert_row(const Rapid_load_context *context, const uchar *data) {
  Imcu *current_imcu = get_or_create_write_imcu();
  if (!current_imcu) {
    return INVALID_ROW_ID;
  }

  Utils::ColumnMapGuard guard(context->m_table);
  RowBuffer row_data(context->m_table->s->fields);
  row_data.copy_from_mysql_fields(context->m_table->field, context->m_table->s->fields);

  row_id_t local_row_id = current_imcu->insert_row(context, row_data);
  if (local_row_id == INVALID_ROW_ID) {  // imcu is full, then create a new one.
    current_imcu = get_or_create_write_imcu();
    if (!current_imcu) return INVALID_ROW_ID;

    local_row_id = current_imcu->insert_row(context, row_data);
  }

  row_id_t global_row_id = current_imcu->get_start_row() + local_row_id;

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, global_row_id)) return HA_ERR_GENERIC;
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, global_row_id)) return HA_ERR_GENERIC;
    /*
     *In MySQL table index processing, adopting the strategy of "explicit primary key first, unique index as fallback"
     *is reasonable and common practice when determining the primary key. This logic first iterates through the table's
     *key_info array to find an explicit primary key (with HA_PRIMARY_KEY flag), selecting it directly if present. If no
     *explicit primary key exists, it chooses the first suitable unique index (with HA_NOSAME flag) as the de facto
     *primary key.
     */
    break;
  }
  // update statistics
  m_metadata.total_rows.fetch_add(1);

  return global_row_id;
}

row_id_t Normal_Table::insert_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                                  size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) {
  ut_a(context->m_table->s->fields == n_cols);

  Imcu *current_imcu = get_or_create_write_imcu();
  if (!current_imcu) {
    return INVALID_ROW_ID;
  }

  Utils::ColumnMapGuard guard(context->m_table);
  RowBuffer row_data(context->m_table->s->fields);
  row_data.copy_from_mysql_fields(context->m_table->field, rowdata, len, col_offsets, n_cols, null_byte_offsets,
                                  null_bitmasks);

  row_id_t local_row_id = current_imcu->insert_row(context, row_data);
  if (local_row_id == INVALID_ROW_ID) {  // imcu is full, then create a new one.
    current_imcu = get_or_create_write_imcu();
    if (!current_imcu) return INVALID_ROW_ID;

    local_row_id = current_imcu->insert_row(context, row_data);
  }

  row_id_t global_row_id = current_imcu->get_start_row() + local_row_id;

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, global_row_id)) return HA_ERR_GENERIC;
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, global_row_id)) return HA_ERR_GENERIC;
    break;
  }

  // update statistics
  m_metadata.total_rows.fetch_add(1);

  return global_row_id;
}

int Normal_Table::delete_row(const Rapid_load_context *context, row_id_t global_row_id) {
  // 1. locate IMCU
  Imcu *imcu = locate_imcu(global_row_id);
  if (!imcu) return HA_ERR_KEY_NOT_FOUND;

  // 2. calc row_id
  assert((imcu->get_start_row() % m_metadata.rows_per_imcu) == 0);
  row_id_t local_row_id = global_row_id - imcu->get_start_row();

  // 3. delete row from IMCU.
  auto success = imcu->delete_row(context, local_row_id);

  if (success) return success;  // return on error.

  // 4. update statistics if delete operation succeeded.
  m_metadata.deleted_rows.fetch_add(1);
  m_metadata.version_count.fetch_add(1);

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Normal_Table::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &row_ids) {
  // 1. the IMCU candidate group.
  std::unordered_map<Imcu *, std::vector<row_id_t>> imcu_groups;

  for (row_id_t global_row_id : row_ids) {
    Imcu *imcu = locate_imcu(global_row_id);
    if (imcu) {
      row_id_t local_row_id = global_row_id - imcu->get_start_row();
      imcu_groups[imcu].push_back(local_row_id);
    }
  }

  // 2. delete rows in IMCU.
  size_t total_deleted = 0;

  for (auto &[imcu, local_ids] : imcu_groups) total_deleted += imcu->delete_rows(context, local_ids);

  // 3. update statistics.
  m_metadata.deleted_rows.fetch_add(total_deleted);

  return total_deleted;
}

int Normal_Table::delete_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                             size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) {
  auto ret{ShannonBase::SHANNON_SUCCESS};
  auto global_row_id = locate_row(context, rowdata, len, col_offsets, n_cols, null_byte_offsets, null_bitmasks);
  if (global_row_id == INVALID_ROW_ID) return HA_ERR_KEY_NOT_FOUND;

  if ((ret = delete_row(context, global_row_id))) {
    std::string errmsg;
    errmsg.append("delete from rapid ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return ret;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Normal_Table::update_row(const Rapid_load_context *context, row_id_t global_row_id,
                             const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates) {
  // 1. locate IMCU.
  Imcu *imcu = locate_imcu(global_row_id);
  if (!imcu) return false;

  // 2. calc row_id.
  row_id_t local_row_id = global_row_id - imcu->get_start_row();

  // 3. update.
  return imcu->update_row(context, local_row_id, updates);
}

row_id_t Normal_Table::locate_row(const Rapid_load_context *context, const uchar *rowdata /*record[0]*/) {
  std::string sch_tb_name = context->m_schema_name;
  sch_tb_name.append(":").append(context->m_table_name);

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)rowdata);  // to set DB_ROW_ID.
    if (build_key_info(context, nullptr, const_cast<uchar *>(rowdata), nullptr, nullptr, nullptr))
      return HA_ERR_GENERIC;
  }

  Utils::ColumnMapGuard guard(context->m_table);
  context->m_table->record[0] = const_cast<uchar *>(rowdata);
  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_key_info(context, key_info)) return HA_ERR_GENERIC;
    break;
  }
  auto rowid = m_indexes[ShannonBase::SHANNON_PRIMARY_KEY_NAME].get()->lookup(context->m_extra_info.m_key_buff.get(),
                                                                              context->m_extra_info.m_key_len);
  auto global_row_id = rowid ? *rowid : INVALID_ROW_ID;
  return global_row_id;
}

row_id_t Normal_Table::locate_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                                  size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) {
  std::string sch_tb_name = context->m_schema_name;
  sch_tb_name.append(":").append(context->m_table_name);

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)rowdata);  // to set DB_ROW_ID.
    if (build_key_info(context, nullptr, rowdata, nullptr, nullptr, nullptr)) return HA_ERR_GENERIC;
  }

  Utils::ColumnMapGuard guard(context->m_table);
  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_key_info(context, key_info, rowdata, col_offsets, null_byte_offsets, null_bitmasks))
      return HA_ERR_GENERIC;
    break;
  }
  auto rowid = m_indexes[ShannonBase::SHANNON_PRIMARY_KEY_NAME].get()->lookup(context->m_extra_info.m_key_buff.get(),
                                                                              context->m_extra_info.m_key_len);
  auto global_row_id = rowid ? *rowid : INVALID_ROW_ID;
  return global_row_id;
}

int Normal_Table::scan_table(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                             const std::vector<uint32_t> &projection, RowCallback callback) {
  // 1. travel all IMCUs.
  for (auto &imcu : m_imcus) {
    // 1.1 Storage Index to filter（skip IMCU ）
    if (imcu->can_skip_imcu(predicates)) {
      continue;  // skip IMCU
    }

    // 1.2 scan IMCU
    imcu->scan(context, predicates, projection, callback);

    // 1.3 check LIMIT oper.
    if (context->limit > 0 && context->rows_returned >= context->limit) {
      break;
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

bool Normal_Table::read(Rapid_scan_context *context, const uchar *key_value, Row_Result &result) { return false; }

bool Normal_Table::range_scan(Rapid_scan_context *context, const uchar *start_key, const uchar *end_key,
                              RowCallback callback) {
  assert(context && start_key && end_key);
  return false;
}

uint64_t Normal_Table::get_row_count(const Rapid_scan_context *context) const {
  assert(context);

  return 0;
}

ColumnStatistics Normal_Table::get_column_stats(uint32_t col_idx) const {
  assert(col_idx);

  ColumnStatistics col_stat(col_idx, "col_name", MYSQL_TYPE_NULL);
  return col_stat;
}

void Normal_Table::update_statistics(bool force) {
  if (force) {
  }
}

size_t Normal_Table::garbage_collect(uint64_t min_active_scn) {
  size_t total_freed = 0;

  // 1. perform GC on each IMCU.
  for (auto &imcu : m_imcus) {
    total_freed += imcu->garbage_collect(min_active_scn);
  }

  // 2. update global version count.
  m_metadata.version_count.fetch_sub(total_freed);

  return total_freed;
  return 0;
}

size_t Normal_Table::compact_imcus(double delete_ratio_threshold) {
  size_t total_freed = 0;

  std::vector<std::shared_ptr<Imcu>> new_imcus;

  for (auto &imcu : m_imcus) {
    if (imcu->needs_compaction() && imcu->get_delete_ratio() >= delete_ratio_threshold) {
      // compact the IMCU.
      auto compacted = imcu->compact();
      if (compacted) {
        new_imcus.emplace_back(compacted);
        total_freed += imcu->estimate_size() - compacted->estimate_size();
      } else {
        new_imcus.emplace_back(imcu);
      }
    } else {
      new_imcus.emplace_back(imcu);
    }
  }

  // change atomically.
  {
    std::unique_lock lock(m_table_mutex);
    m_imcus = std::move(new_imcus);
    build_imcu_index();
  }

  return total_freed;
}

bool Normal_Table::reorganize() { return false; }

Imcu *Normal_Table::get_or_create_write_imcu() {
  Imcu *current = m_current_imcu.load();

  if (current && !current->is_full()) {
    return current;
  }

  /**
   * EACH MCU CONTAINS `SHANNON_ROWS_IN_CHUNK` (DEFAULT) ROWS.
   */
  std::unique_lock lock(m_table_mutex);
  row_id_t start_row = m_imcus.empty() ? 0 : m_imcus.size() * m_metadata.rows_per_imcu;

  auto new_imcu = std::make_shared<Imcu>(this, m_metadata, start_row /*start_row_#*/,
                                         m_metadata.rows_per_imcu /*capacity*/, m_memory_pool);
  m_imcus.push_back(new_imcu);
  m_current_imcu.store(new_imcu.get());

  update_imcu_index(new_imcu.get());

  return new_imcu.get();
}
}  // namespace Imcs
}  // namespace ShannonBase
