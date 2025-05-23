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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_COMPRESS_DICTIONARY_H__
#define __SHANNONBASE_COMPRESS_DICTIONARY_H__

#include <mutex>
#include <unordered_map>

#include "include/my_inttypes.h"
#include "include/mysql/strings/m_ctype.h"  //CHARSET_INFO
#include "include/sql_string.h"             //String

namespace ShannonBase {
namespace Compress {

enum class Encoding_type : uint8 { NONE, SORTED, VARLEN };
// Dictionary, which store all the dictionary data.
class Dictionary {
 public:
  static constexpr auto DEFAULT_STRID = 0;
  static constexpr auto INVALID_STRID = -1;

  Dictionary(Encoding_type type) : m_encoding_type(type) {
    m_content.emplace("unknown", 0);
    m_id2content.emplace(0, "unknown");
  }
  Dictionary() = delete;
  virtual ~Dictionary() = default;

  // store the string into dictionary, and return the string id.
  virtual uint32 store(const uchar *, size_t, Encoding_type type = Encoding_type::NONE);

  // get the string by string id. surcess return 0 and string, otherwise not found return -1.;
  virtual int32 get(uint64 strid, String &ret_val);

  // get the string id by string. if not found, return empty string.
  virtual std::string get(uint64 strid);

  // get the string id by string. if not found, return -1.
  virtual int64 get(const std::string &str);

  // set the algorithm type.
  virtual void set_algo(Encoding_type type) { m_encoding_type = type; }

  // get the algorithm type.
  virtual inline Encoding_type get_algo() const { return m_encoding_type; }

  // get the content size.
  virtual inline uint32 content_size() const { return m_content.size(); }

 private:
  std::mutex m_content_mtx;

  // the encoding type of this dictionary used.
  Encoding_type m_encoding_type{Encoding_type::NONE};

  // compressed cotent string mapp, key: compressed string, value: compressed
  // string id in this map.
  std::unordered_map<std::string, uint64> m_content;

  // string id<--> original string. for access accleration.
  std::unordered_map<uint64, std::string> m_id2content;
};

}  // namespace Compress
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COMPRESS_DICTIONARY_H__