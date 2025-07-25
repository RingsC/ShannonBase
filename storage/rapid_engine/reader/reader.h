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
#ifndef __SHANNONBASE_READER_READER_H__
#define __SHANNONBASE_READER_READER_H__
#include <string>

#include "include/my_inttypes.h"
#include "sql/sql_lex.h"
#include "storage/rapid_engine/include/rapid_object.h"
class key_range;
namespace ShannonBase {
namespace Reader {
// interface of reader, which is used to travel all data.
class Reader : public MemoryObject {
 public:
  Reader() = default;
  virtual ~Reader() = default;
  virtual int open() = 0;
  virtual int close() = 0;
  virtual int read(Secondary_engine_execution_context *, uchar *, size_t = 0) = 0;
  virtual int records_in_range(Secondary_engine_execution_context *, unsigned int, key_range *, key_range *) = 0;
  virtual int write(Secondary_engine_execution_context *, uchar *, size_t = 0) = 0;
  virtual int index_read(Secondary_engine_execution_context *, uchar *, uchar *, uint, ha_rkey_function) = 0;
  virtual int index_general(Secondary_engine_execution_context *, uchar *, size_t = 0) = 0;
  virtual int index_next(Secondary_engine_execution_context *, uchar *, size_t = 0) = 0;
  virtual int index_next_same(Secondary_engine_execution_context *, uchar *, uchar *, uint, ha_rkey_function) = 0;
  virtual uchar *tell(uint = 0) = 0;
  virtual uchar *seek(size_t offset) = 0;
};

}  // namespace Reader
}  // namespace ShannonBase
#endif  //__SHANNONBASE_READER_READER_H__