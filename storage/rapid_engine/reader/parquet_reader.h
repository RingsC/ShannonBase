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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_PARQUET_READER_H__
#define __SHANNONBASE_PARQUET_READER_H__
#include <string>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include "include/my_inttypes.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/reader/reader.h"

class key_range;
namespace ShannonBase {
namespace Reader {
class ParquetReader : public Reader {
 public:
  ParquetReader(std::string &path) : m_path(path), m_current_row(0), m_total_rows(0) {}
  ParquetReader() = delete;
  virtual ~ParquetReader() {}

  int open() override;
  int close() override;
  int read(Secondary_engine_execution_context *context, uchar *buffer, size_t length = 0) override;
  int write(Secondary_engine_execution_context *context, uchar *buffer, size_t length = 0) override;
  int records_in_range(Secondary_engine_execution_context *context, unsigned int index, key_range *min,
                       key_range *max) override;
  int index_read(Secondary_engine_execution_context *context, uchar *buff, uchar *key, uint key_len,
                 ha_rkey_function find_flag) override;
  int index_next(Secondary_engine_execution_context *context, uchar *buff, size_t length = 0) override;
  int index_next_same(Secondary_engine_execution_context *context, uchar *buff, uchar *key, uint key_len,
                      ha_rkey_function find_flag) override;
  int index_general(Secondary_engine_execution_context *context, uchar *buff, size_t length = 0) override;

  uchar *tell(uint field_index = 0) override;
  uchar *seek(size_t offset) override;

 private:
  std::string m_path;
  std::shared_ptr<parquet::FileReader> m_file_reader;
  std::shared_ptr<arrow::Table> m_table;
  std::shared_ptr<arrow::Schema> m_schema;

  size_t m_current_row;
  size_t m_total_rows;

  int load_table();
  int convert_row_to_buffer(size_t row_index, uchar *buffer, size_t buffer_length);
  size_t get_row_size();
  bool is_valid_row_index(size_t row_index) const;
};
}  // namespace Reader
}  // namespace ShannonBase
#endif  //__SHANNONBASE_PARQUET_READER_H__