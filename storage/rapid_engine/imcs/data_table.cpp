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

/**DataTable to mock a table hehaviors. We can use a DataTable to open the IMCS
 * with sepecific table information. After the Cu belongs to this table were found
 * , we can use this DataTable object to read/write, etc., just like a normal innodb
 * table.
 */
#include "storage/rapid_engine/imcs/data_table.h"
#include <sstream>

#include "include/ut0dbg.h"  //ut_a
#include "sql/table.h"       //TABLE

#include "storage/rapid_engine/imcs/chunk.h"  //CHUNK
#include "storage/rapid_engine/imcs/cu.h"     //CU
#include "storage/rapid_engine/imcs/imcs.h"   //IMCS

namespace ShannonBase {
namespace Imcs {

DataTable::DataTable(TABLE *source_table) : m_data_source(source_table) { ut_a(m_data_source); }

DataTable::~DataTable() {}

int DataTable::open() { return 0; }

int DataTable::close() { return 0; }

int DataTable::init() {
  if (!m_initialized.load()) {
    m_initialized.store(true);
    scan_init();
  }

  return 0;
}

void DataTable::scan_init() {
  std::ostringstream key_part, key;
  key_part << m_data_source->s->db.str << ":" << m_data_source->s->table_name.str << ":";
  for (auto index = 0u; index < m_data_source->s->fields; index++) {
    auto fld = *(m_data_source->field + index);
    key << key_part.str() << fld->field_name;
    auto key_str = key.str();

    m_field_cus.push_back(Imcs::instance()->get_cu(key_str));
    key.str("");
  }
  ut_a(m_field_cus.size() == m_data_source->s->fields);
  m_rowid.store(0);

#ifndef NDEBUG
  auto first_num = m_field_cus[0]->prows();
  for (auto &item : m_field_cus) {
    ut_a(first_num == item->prows());
  }
#endif
}

int DataTable::next(uchar *buf) {
// make all ptr in m_field_ptrs to move forward one step(one row).
start_pos:
  if (m_rowid >= m_field_cus[0]->prows()) return HA_ERR_END_OF_FILE;

  for (auto idx = 0u; idx < m_field_cus.size(); idx++) {
    auto cu = m_field_cus[idx];
    auto normalized_length = cu->normalized_pack_length();
    bool is_text_value{false};

    switch (cu->header()->m_source_fld->type()) {
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
        /**if this is a string type, it will be use local dictionary encoding, therefore,
         * using stringid as field value. */
        is_text_value = true;
        break;
      default:
        break;
    }

    DBUG_EXECUTE_IF("secondary_engine_rapid_next_error", {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
      return HA_ERR_GENERIC;
    });

    auto source_fld = *(m_data_source->field + idx);
    ut_a(source_fld->field_index() == cu->header()->m_source_fld->field_index());
    auto current_chunk = m_rowid / SHANNON_ROWS_IN_CHUNK;
    auto offset_in_chunk = m_rowid % SHANNON_ROWS_IN_CHUNK;
    bool null_flag = cu->chunk(current_chunk)->is_null(offset_in_chunk);
    null_flag ? source_fld->set_null() : source_fld->set_notnull();
    // TODO: to check version link to check its old value.
    if (cu->chunk(current_chunk)->is_deleted(offset_in_chunk)) {
      m_rowid.fetch_add(1);
      goto start_pos;
    }

    auto old_map = tmp_use_all_columns(m_data_source, m_data_source->write_set);
    auto data_ptr = cu->chunk(current_chunk)->base() + offset_in_chunk * normalized_length;
    if (is_text_value) {
      uint32 str_id = *(uint32 *)data_ptr;
      source_fld->set_field_ptr(cu->header()->m_local_dict->get(str_id));
    } else
      source_fld->set_field_ptr(data_ptr);
    if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
  }

  m_rowid.fetch_add(1);
  return 0;
}

int DataTable::end() {
  m_rowid.store(0);
  return 0;
}

row_id_t DataTable::find(uchar *buf) {
  row_id_t rowid{0u};
  return rowid;
}

}  // namespace Imcs
}  // namespace ShannonBase