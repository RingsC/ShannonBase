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
#include "storage/rapid_engine/imcs/imcs.h"

#include <threads.h>
#include <sstream>
#include <string>

#include "include/decimal.h"
#include "include/my_dbug.h"                     //DBUG_EXECUTE_IF
#include "sql/partitioning/partition_handler.h"  //partition handler

#include "storage/innobase/handler/ha_innopart.h"
#include "storage/innobase/include/data0type.h"
#include "storage/innobase/include/mach0data.h"
#include "storage/innobase/include/univ.i"    //UNIV_SQL_NULL
#include "storage/innobase/include/ut0dbg.h"  //ut_ad
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/populate.h"
#include "storage/rapid_engine/utils/utils.h"  //Utils

namespace ShannonBase {
namespace Imcs {

Imcs *Imcs::m_instance{nullptr};
std::once_flag Imcs::one;

SHANNON_THREAD_LOCAL Imcs *current_imcs_instance = Imcs::instance();

int Imcs::initialize() {
  m_inited.store(1);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::deinitialize() {
  m_inited.store(0);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_user_index_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  std::string keypart, keyname;
  keypart.append(source->s->db.str).append(":").append(source->s->table_name.str).append(":");

  if (context->m_extra_info.m_partition_infos.size()) {
    for (auto &part : context->m_extra_info.m_partition_infos) {
      auto partkey = part.first;
      partkey.append("-").append(std::to_string(part.second)).append(":");
      keyname.append(keypart).append(partkey);

      for (auto ind = 0u; ind < source->s->keys; ind++) {
        auto key_info = source->key_info + ind;
        std::vector<std::string> key_parts_names;
        keyname.append(key_info->name).append(":");

        for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
          std::string key;
          key.append(keyname).append(key_info->key_part[i].field->field_name);
          key_parts_names.push_back(key);
        }
        m_source_keys.emplace(keyname, std::make_pair(key_info->key_length, key_parts_names));
        m_indexes.emplace(keyname, std::make_unique<Index::Index<uchar, row_id_t>>(keyname));
      }
      keyname.clear();
    }
  } else {
    for (auto ind = 0u; ind < source->s->keys; ind++) {
      auto key_info = source->key_info + ind;
      std::vector<std::string> key_parts_names;
      std::string keykeypart(keypart);
      keykeypart.append(key_info->name).append(":");
      for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
        std::string key;
        key.append(keykeypart).append(key_info->key_part[i].field->field_name);
        key_parts_names.push_back(key);
      }
      m_source_keys.emplace(keykeypart, std::make_pair(key_info->key_length, key_parts_names));
      m_indexes.emplace(keykeypart, std::make_unique<Index::Index<uchar, row_id_t>>(keykeypart));
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_sys_index_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  std::string keypart;
  keypart.append(source->s->db.str).append(":").append(source->s->table_name.str).append(":");

  std::string keykeypart, keyname;
  if (context->m_extra_info.m_partition_infos.size()) {
    for (auto &part : context->m_extra_info.m_partition_infos) {
      auto partkey = part.first;
      partkey.append("-").append(std::to_string(part.second)).append(":");
      keykeypart.append(keypart).append(partkey);
      keykeypart.append(ShannonBase::SHANNON_PRIMARY_KEY_NAME).append(":");
      keyname.append(keykeypart).append(SHANNON_DB_ROW_ID);

      m_source_keys.emplace(keykeypart, std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{keyname}));
      m_indexes.emplace(keykeypart, std::make_unique<Index::Index<uchar, row_id_t>>(keyname));
      keykeypart.clear();
    }
  } else {  // no partitioned table.
    keykeypart.append(keypart).append(ShannonBase::SHANNON_PRIMARY_KEY_NAME).append(":");
    keyname.append(keykeypart).append(SHANNON_DB_ROW_ID);
    m_source_keys.emplace(keykeypart, std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{keyname}));
    m_indexes.emplace(keykeypart, std::make_unique<Index::Index<uchar, row_id_t>>(keyname));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_index_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  std::string keypart;
  keypart.append(source->s->db.str).append(":").append(source->s->table_name.str).append(":");

  // build index info for every Cus.
  // no.1: primary key. using row_id as the primary key when missing user-defined pk.
  if (source->s->is_missing_primary_key()) create_sys_index_memo(context, source);

  // no.2: user-defined indexes.
  create_user_index_memo(context, source);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_table_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  std::string keypart;
  keypart.append(source->s->db.str).append(":").append(source->s->table_name.str).append(":");

  // step 1: build the Cus meta info for every column.
  for (auto index = 0u; index < source->s->fields; index++) {
    std::string key;
    auto fld = *(source->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (context->m_extra_info.m_partition_infos.size()) {
      for (auto &part : context->m_extra_info.m_partition_infos) {
        auto part_name = part.first;
        part_name.append("-").append(std::to_string(part.second)).append(":");
        key.append(keypart).append(part_name).append(fld->field_name);
        m_cus.emplace(key, std::make_unique<Cu>(fld, key));
        key.clear();
      }
    } else {
      key.append(keypart).append(fld->field_name);
      m_cus.emplace(key, std::make_unique<Cu>(fld, key));
      key.clear();
    }
  }

  // step 2: build indexes.
  return create_index_memo(context, source);
  /* in secondary load phase, the table not loaded into imcs. therefore, it can be seen
     by any transactions. If this table has been loaded into imcs. A new data such as
     insert/update/delete will associated with a SMU items to trace its visibility. Therefore
     the following DB_TRX_ID cu no need to build.
  key.clear();
  std::unique_ptr<Mock_field_trxid> trx_fld = std::make_unique<Mock_field_trxid>();
  trx_fld.get()->table = const_cast<TABLE *>(source);
  key.append(keypart).append(SHANNON_DB_TRX_ID);
  m_cus.emplace(key, std::make_unique<Cu>(trx_fld.get()));
  */
}

Cu *Imcs::at(std::string_view schema, std::string_view table, size_t index) {
  if (index >= m_cus.size()) return nullptr;

  std::string keypart{schema};
  keypart.append(":").append(table).append(":");
  size_t count = 0;

  auto it = std::find_if(m_cus.begin(), m_cus.end(),
                         [&](const auto &pair) { return pair.first.find(keypart) == 0 && count++ == index; });

  return (it != m_cus.end()) ? it->second.get() : nullptr;
}

Cu *Imcs::get_cu(std::string_view key) {
  std::shared_lock<std::shared_mutex> lk(m_cus_mutex);
  std::string key_str(key);
  if (m_cus.find(key_str) == m_cus.end()) return nullptr;
  return m_cus[key_str].get();
}

Index::Index<uchar, row_id_t> *Imcs::get_index(std::string_view key) {
  std::string key_str(key);

  if (m_indexes.find(key_str) == m_indexes.end()) return nullptr;
  return m_indexes[key_str].get();
}

int Imcs::build_index_impl(const Rapid_load_context *context, const TABLE *source, const KEY *key, row_id_t rowid) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto keypart = std::string(source->s->db.str);
  keypart.append(":").append(source->s->table_name.str).append(":");

  if (key == nullptr) {
    /* No primary key was defined for the table and we
    generated the clustered index from row id: the
    row reference will be the row id, not any key value
    that MySQL knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    keypart.append(ShannonBase::SHANNON_PRIMARY_KEY_NAME).append(":");
    ut_a(const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len == source->file->ref_length);
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    keypart.append(key->name).append(":");
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();

    uint length{0u};
    KEY_PART_INFO *key_part;
    /* Copy the key parts */
    auto key_length = key->key_length;
    for (key_part = key->key_part; (int)key_length > 0; key_part++) {
      if (key_part->null_bit) {
        bool key_is_null = from_record[key_part->null_offset] & key_part->null_bit;
        *to_key++ = (key_is_null ? 1 : 0);
        key_length--;
      }

      if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
        key_length -= HA_KEY_BLOB_LENGTH;
        length = std::min<uint>(key_length, key_part->length);
        key_part->field->get_key_image(to_key, length, Field::itRAW);
        to_key += HA_KEY_BLOB_LENGTH;
      } else {
        length = std::min<uint>(key_length, key_part->length);
        Field *field = key_part->field;
        const CHARSET_INFO *cs = field->charset();
        if (field->type() == MYSQL_TYPE_DOUBLE || field->type() == MYSQL_TYPE_FLOAT ||
            field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL) {
          uchar encoding[8] = {0};
          Utils::Encoder<double>::EncodeFloat(field->val_real(), encoding);
          memcpy(to_key, encoding, length);
        } else {
          const size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
          if (bytes < length) cs->cset->fill(cs, (char *)to_key + bytes, length - bytes, ' ');
        }
      }
      to_key += length;
      key_length -= length;
    }
  }

  m_indexes[keypart]->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                             sizeof(rowid));
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

int Imcs::build_index(const Rapid_load_context *context, const TABLE *source, const KEY *key, row_id_t rowid) {
  auto ret = build_index_impl(context, source, key, rowid);
  return ret;
}

int Imcs::build_indexes_from_keys(const Rapid_load_context *context, std::map<std::string, key_info_t> &keys,
                                  row_id_t rowid) {
  for (auto &key : keys) {
    auto key_name = key.first;
    auto key_len = key.second.first;
    auto key_buff = key.second.second.get();
    ut_a(m_indexes.find(key_name) != m_indexes.end());
    m_indexes[key_name].get()->insert(key_buff, key_len, &rowid, sizeof(row_id_t));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::build_indexes_from_log(const Rapid_load_context *context, std::map<std::string, mysql_field_t> &field_values,
                                 row_id_t rowid) {
  auto keypart = std::string(context->m_schema_name);
  keypart.append(":").append(context->m_table_name).append(":");

  auto matched_keys = source_key(keypart);
  ut_a(matched_keys.size() > 0);

  std::unique_ptr<uchar[]> key_buff{nullptr};

  for (auto &key : matched_keys) {
    auto key_name = key.first;
    auto key_info = key.second;
    key_buff.reset(new uchar[key_info.first]);
    memset(key_buff.get(), 0x0, key_info.first);
    uint key_offset{0u};
    for (auto &keykey : key_info.second) {
      auto parts = ShannonBase::Utils::Util::split(keykey, ':');
      ut_a(parts.size() >= 4);
      auto fldfld = parts[0] + ":" + parts[1] + ":" + parts[3];

      ut_a(field_values.find(fldfld) != field_values.end());
      if (field_values[fldfld].has_nullbit) {
        *key_buff.get() = (field_values[fldfld].is_null) ? 1 : 0;
        key_offset++;
      }

      auto cs = Imcs::Imcs::instance()->get_cu(fldfld) ? Imcs::Imcs::instance()->get_cu(fldfld)->header()->m_charset
                                                       : nullptr;
      if (field_values[fldfld].mtype == DATA_BLOB || field_values[fldfld].mtype == DATA_VARCHAR ||
          field_values[fldfld].mtype == DATA_VARMYSQL) {
        int2store(key_buff.get() + key_offset, field_values[fldfld].plength);
        key_offset += HA_KEY_BLOB_LENGTH;
        std::memcpy(key_buff.get() + key_offset, field_values[fldfld].data.get(), field_values[fldfld].mlength);
        key_offset += field_values[fldfld].mlength;
      } else {
        ut_a(field_values[fldfld].mlength = field_values[fldfld].plength);
        if (field_values[fldfld].mtype == DATA_DOUBLE || field_values[fldfld].mtype == DATA_FLOAT ||
            field_values[fldfld].mtype == DATA_DECIMAL) {
          ut_a(field_values[fldfld].mlength == 8);
          uchar encoding[8] = {0};
          auto val = *(double *)field_values[fldfld].data.get();
          Utils::Encoder<double>::EncodeFloat(val, encoding);
          std::memcpy(key_buff.get() + key_offset, encoding, field_values[fldfld].mlength);
          key_offset += field_values[fldfld].mlength;
        } else {
          std::memcpy(key_buff.get() + key_offset, field_values[fldfld].data.get(), field_values[fldfld].mlength);
          key_offset += field_values[fldfld].mlength;
          if (key_offset < key_info.first && cs)
            cs->cset->fill(cs, (char *)key_buff.get() + key_offset, key_info.first - key_offset, ' ');
        }
      }
    }

    ut_a(m_indexes.find(key_name) != m_indexes.end());
    m_indexes[key_name].get()->insert(key_buff.get(), key_info.first, &rowid, sizeof(row_id_t));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::fill_record(const Rapid_load_context *context, std::string &current_key, handler *file) {
  /**
   * for VARCHAR type Data in field->ptr is stored as: 1 or 2 bytes length-prefix-header  (from
   * Field_varstring::length_bytes) data. the here we dont use var_xxx to get data, rather getting
   * directly, due to we dont care what real it is. ref to: field.cc:6703
   */
  std::string key;
  row_id_t rowid{0};
  for (auto index = 0u; index < context->m_table->s->fields; index++) {
    auto fld = *(context->m_table->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    key.append(current_key).append(fld->field_name);
    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};
    if (Utils::Util::is_blob(fld->type())) {
      data_ptr = const_cast<uchar *>(fld->data_ptr());
      data_len = down_cast<Field_blob *>(fld)->get_length();
    } else {
      extra_offset = Utils::Util::is_varstring(fld->type()) ? (fld->field_length > 256 ? 2 : 1) : 0;
      data_ptr = fld->is_null() ? nullptr : fld->field_ptr() + extra_offset;
      if (fld->is_null()) {
        data_len = UNIV_SQL_NULL;
        data_ptr = nullptr;
      } else {
        if (extra_offset == 1)
          data_len = mach_read_from_1(fld->field_ptr());
        else if (extra_offset == 2)
          data_len = mach_read_from_2_little_endian(fld->field_ptr());
        else
          data_len = fld->pack_length();
      }
    }

    if (!m_cus[key]->write_row(context, data_ptr, data_len)) {
      return HA_ERR_GENERIC;
    }
    rowid = m_cus[key]->header()->m_prows.load(std::memory_order_relaxed) - 1;
    key.clear();
  }

  if (context->m_table->s->is_missing_primary_key()) {
    file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, context->m_table, nullptr, rowid)) return HA_ERR_GENERIC;

    for (auto index = 0u; index < context->m_table->s->keys; index++) {
      auto key_info = context->m_table->key_info + index;
      if (build_index(context, context->m_table, key_info, rowid)) return HA_ERR_GENERIC;
    }
  } else {
    for (auto index = 0u; index < context->m_table->s->keys; index++) {
      auto key_info = context->m_table->key_info + index;
      if (build_index(context, context->m_table, key_info, rowid)) return HA_ERR_GENERIC;
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodb(const Rapid_load_context *context, ha_innobase *file) {
  auto m_thd = context->m_thd;
  auto sch_name = context->m_schema_name.c_str();
  auto table_name = context->m_table_name.c_str();
  // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
  if (file->inited == handler::NONE && file->ha_rnd_init(true)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), sch_name, table_name);
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  m_thd->set_sent_row_count(0);
  std::string key_part;
  key_part.append(sch_name).append(":").append(table_name).append(":");
  while ((tmp = file->ha_rnd_next(context->m_table->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
      my_error(ER_SECONDARY_ENGINE, MYF(0), sch_name, table_name);
      file->ha_rnd_end();
      return HA_ERR_GENERIC;
    });

    // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
    auto load_context = const_cast<Rapid_load_context *>(context);
    load_context->m_extra_info.m_key_len = file->ref_length;
    if (fill_record(context, key_part, file)) {
      file->ha_rnd_end();
      std::string errmsg;
      errmsg.append("load data from ").append(sch_name).append(".").append(table_name).append(" to imcs failed.");
      my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    }

    m_thd->inc_sent_row_count(1);

    if (tmp == HA_ERR_RECORD_DELETED && !m_thd->killed) continue;
  }
  // end of load the data from innodb to imcs.
  file->ha_rnd_end();

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodbpart(const Rapid_load_context *context, ha_innopart *file) {
  auto m_thd = context->m_thd;

  auto sch_name = context->m_schema_name.c_str();
  auto table_name = context->m_table_name.c_str();
  std::string key_part;
  key_part.append(sch_name).append(":").append(table_name).append(":");

  for (auto &part : context->m_extra_info.m_partition_infos) {
    m_thd->set_sent_row_count(0);
    auto partkey = key_part;
    partkey.append(part.first).append("-").append(std::to_string(part.second)).append(":");
    // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
    if (file->inited == handler::NONE && file->rnd_init_in_part(part.second, true)) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), sch_name, table_name);
      return HA_ERR_GENERIC;
    }

    int tmp{HA_ERR_GENERIC};
    while ((tmp = file->rnd_next_in_part(part.second, context->m_table->record[0])) != HA_ERR_END_OF_FILE) {
      /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
       without locks. Now, do full scan, but multi-thread scan will impl in future. */
      if (tmp == HA_ERR_KEY_NOT_FOUND) break;

      DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
        my_error(ER_SECONDARY_ENGINE, MYF(0), sch_name, table_name);
        file->rnd_end_in_part(part.second, true);
        return HA_ERR_GENERIC;
      });

      // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
      auto load_context = const_cast<Rapid_load_context *>(context);
      load_context->m_extra_info.m_key_len = file->ref_length;
      if (fill_record(context, partkey, file)) {
        file->rnd_end_in_part(part.second, true);
        std::string errmsg;
        errmsg.append("load data from ").append(sch_name).append(".").append(table_name).append(" to imcs failed.");
        my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
      }
      if (tmp == HA_ERR_RECORD_DELETED && !m_thd->killed) continue;
    }
    // end of load the data from innodb to imcs.
    file->rnd_end_in_part(part.second, true);
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_table(const Rapid_load_context *context, const TABLE *source) {
  if (create_table_memo(context, source)) {
    std::string errmsg;
    errmsg.append("create table memo for ")
        .append(context->m_schema_name)
        .append(".")
        .append(context->m_table_name)
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  auto ret{ShannonBase::SHANNON_SUCCESS};
  if (context->m_thd->lex->query_block->get_table_list()->partition_names && source->file->get_partition_handler()) {
    if (load_innodbpart(context, dynamic_cast<ha_innopart *>(source->file))) {
      // if load partition table failed, then do normal load mode, therefore clear partition info.
      const_cast<Rapid_load_context *>(context)->m_extra_info.m_partition_infos.clear();
      ret = load_innodb(context, dynamic_cast<ha_innobase *>(source->file));
    }
  } else
    ret = load_innodb(context, dynamic_cast<ha_innobase *>(source->file));
  return ret;
}

int Imcs::unload_cus(const Rapid_load_context *context, std::string &keyname, bool error_if_not_loaded) {
  auto found{false};
  // step 1: unload CUs.
  std::unique_lock<std::shared_mutex> lk(m_cus_mutex);
  for (auto it = m_cus.begin(); it != m_cus.end();) {
    if (it->first.find(keyname) != std::string::npos) {
      it = m_cus.erase(it);
      found = true;
    } else
      ++it;
  }
  if (error_if_not_loaded && !found) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_indexes(const Rapid_load_context *context, std::string &keyname, bool error_if_not_loaded) {
  // step 1: unload source key meta infos.
  bool found{false};
  for (auto it = m_source_keys.begin(); it != m_source_keys.end();) {
    if (it->first.find(keyname) != std::string::npos) {
      it = m_source_keys.erase(it);
      found = true;
    } else
      ++it;
  }

  if (error_if_not_loaded && !found) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  // step 2: unload the keys.
  found = false;
  for (auto it = m_indexes.begin(); it != m_indexes.end();) {
    if (it->first.find(keyname) != std::string::npos) {
      it = m_indexes.erase(it);
      found = true;
    } else
      ++it;
  }

  if (error_if_not_loaded && !found) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_innodb(const Rapid_load_context *context, const char *db_name, const char *table_name,
                        bool error_if_not_loaded) {
  std::string keypart;
  keypart.append(db_name).append(":").append(table_name).append(":");
  // step 1: unload CUs.
  unload_cus(context, keypart, error_if_not_loaded);

  // step 2: unload source key meta infos and indexes.
  unload_indexes(context, keypart, error_if_not_loaded);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_innodbpart(const Rapid_load_context *context, const char *db_name, const char *table_name,
                            bool error_if_not_loaded) {
  std::string keypart(db_name);
  keypart.append(":").append(table_name).append(":");
  for (auto &part : context->m_extra_info.m_partition_infos) {
    auto key_name(keypart);
    key_name.append(part.first).append("-").append(std::to_string(part.second)).append(":");

    // step 1: unload CUs.
    unload_cus(context, key_name, error_if_not_loaded);

    // step 2: unload source key meta infos and indexes.
    unload_indexes(context, key_name, error_if_not_loaded);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                       bool error_if_not_loaded) {
  /** the key format: "db_name:table_name:field_name", all the ghost columns also should be
   *  removed*/
  int ret{ShannonBase::SHANNON_SUCCESS};
  auto partition_hanlder = context->m_table ? context->m_table->file->get_partition_handler() : nullptr;
  auto partition_names = context->m_thd->lex->query_block->get_table_list()
                             ? context->m_thd->lex->query_block->get_table_list()->partition_names
                             : nullptr;
  if (partition_names && partition_hanlder) {
    ret = unload_innodbpart(context, db_name, table_name, error_if_not_loaded);
  } else
    ret = unload_innodb(context, db_name, table_name, error_if_not_loaded);
  return ret;
}

int Imcs::insert_row(const Rapid_load_context *context, row_id_t rowid, uchar *buf) {
  ut_a(context && buf);

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                             std::map<std::string, mysql_field_t> &fields) {
  std::string key_name;
  std::string keypart, trxid_key, rowid_key;
  keypart.append(context->m_schema_name).append(":").append(context->m_table_name).append(":");
  trxid_key.append(keypart).append(SHANNON_DB_TRX_ID);
  rowid_key.append(keypart).append(SHANNON_DB_ROW_ID);

  for (auto &field_val : fields) {
    key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == trxid_key || this->get_cu(key_name) == nullptr) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!this->get_cu(key_name)->write_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);
  if (!m_cus.size()) return SHANNON_SUCCESS;

  std::string keypart;
  keypart.append(context->m_schema_name).append(":").append(context->m_table_name).append(":");
  for (auto it = m_cus.begin(); it != m_cus.end();) {
    if (UNIV_UNLIKELY(!it->second || it->first.find(keypart) == std::string::npos)) {
      ++it;
      continue;
    }

    if (!it->second->delete_row(context, rowid)) {
      return HA_ERR_GENERIC;
    }
    ++it;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) {
  ut_a(context);

  if (!m_cus.size()) return SHANNON_SUCCESS;

  std::string keypart;
  keypart.append(context->m_schema_name).append(":").append(context->m_table_name).append(":");
  if (rowids.empty()) {  // delete all rows.
    for (auto &cu : m_cus) {
      if (cu.first.find(keypart) == std::string::npos) continue;

      assert(cu.second);
      if (!cu.second->delete_row_all(context)) return HA_ERR_GENERIC;
    }

    return ShannonBase::SHANNON_SUCCESS;
  }

  for (auto &rowid : rowids) {
    for (auto &cu : m_cus) {
      if (cu.first.find(keypart) == std::string::npos) continue;

      assert(cu.second);
      if (!cu.second->delete_row(context, rowid)) return HA_ERR_GENERIC;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                     const uchar *new_field_data, size_t nlen) {
  ut_a(context);

  ut_a(m_cus[field_key]);
  auto ret = m_cus[field_key]->update_row(context, rowid, const_cast<uchar *>(new_field_data), nlen);
  if (!ret) return HA_ERR_GENERIC;
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                              std::map<std::string, mysql_field_t> &upd_recs) {
  ut_a(context);

  std::string key_name;
  std::string keypart, trxid_key, rowid_key;
  keypart.append(context->m_schema_name).append(":").append(context->m_table_name).append(":");
  trxid_key.append(keypart).append(SHANNON_DB_TRX_ID);
  rowid_key.append(keypart).append(SHANNON_DB_ROW_ID);

  for (auto &field_val : upd_recs) {
    key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == trxid_key || this->get_cu(key_name) == nullptr) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!this->get_cu(key_name)->update_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::rollback_changes_by_trxid(Transaction::ID trxid) {
  for (auto &cu : m_cus) {
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

}  // namespace Imcs
}  // namespace ShannonBase