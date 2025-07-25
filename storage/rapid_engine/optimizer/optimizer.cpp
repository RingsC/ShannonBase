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

   Copyright (c) 2023 - 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/optimizer/optimizer.h"

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/hash_join_iterator.h"  //HashJoinIterator
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"                      //Query_expression
#include "sql/sql_optimizer.h"                //JOIN
#include "storage/innobase/include/ut0dbg.h"  //ut_a

#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/optimizer/rules/const_fold_rule.h"

#include "storage/rapid_engine/optimizer/path/access_path.h"
#include "storage/rapid_engine/populate/populate.h"

namespace ShannonBase {
namespace Optimizer {

Timer::Timer() { m_begin = std::chrono::steady_clock::now(); }
std::chrono::nanoseconds Timer::lap() {
  const auto now = std::chrono::steady_clock::now();
  const auto lap_duration = std::chrono::nanoseconds{now - m_begin};
  m_begin = now;
  return lap_duration;
}
std::string Timer::lap_formatted() {
  auto stream = std::stringstream{};
  return stream.str();
}

// ctor
Optimizer::Optimizer(std::shared_ptr<Query_expression> &expr, const std::shared_ptr<CostEstimator> &cost_estimator) {}

void OptimzieAccessPath(AccessPath *path, JOIN *join) {
  switch (path->type) {
    // The only supported join type is hash join. Other join types are disabled
    // in handlerton::secondary_engine_flags.
    case AccessPath::TABLE_SCAN: {
      auto table = path->table_scan().table;
      if (table->s->is_secondary_engine() && table->file->stats.records >= SHANNON_VECTOR_WIDTH) {
        // this table is used by query and the table has been loaded into rapid engine. then start
        // a propagation.
        ShannonBase::Populate::Populator::send_notify();
      }
    } break;
    case AccessPath::HASH_JOIN: {
      auto hash_iter [[maybe_unused]] = reinterpret_cast<HashJoinIterator *>(path->iterator);
      assert(hash_iter);
    } break;
    case AccessPath::NESTED_LOOP_JOIN: /* purecov: deadcode */
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
    case AccessPath::BKA_JOIN:
    // Index access is disabled in ha_rapid::table_flags(), so we should see
    // none of these access types.
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      break;
    default:
      break;
  }

  // This secondary storage engine does not yet store anything in the auxiliary
  // data member of AccessPath.
  assert(path->secondary_engine_data == nullptr);
}

// To build the customerized access path（such as: Vectorized Table Scan、GPU Join, etc.）
// For example: add a new AccessPath Type: VECTOR_TABLE_SCAN, then create a new AccessPath
// for this type VectorizedTableSan, then in PathGenerator::CreateIteratorFromAccessPath
// to create its corressponding iterator.
AccessPath *OptimizeAndRewriteAccessPath(AccessPath *path, const JOIN *join) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN: {
      TABLE *table = path->table_scan().table;
      auto secondary_engine = table->s->is_secondary_engine();
      auto can_vectorized = (table->file->stats.records >= SHANNON_VECTOR_WIDTH) ? true : false;

      // create vectorized table scan if it can.
      return AccessPathFactory::CreateTableScan(table, nullptr, secondary_engine && can_vectorized);
    } break;
    case AccessPath::INDEX_SCAN: {
    } break;
    case AccessPath::SAMPLE_SCAN: {
    } break;
    case AccessPath::REF: {
    } break;
    case AccessPath::REF_OR_NULL: {
    } break;
    case AccessPath::EQ_REF: {
    } break;
    case AccessPath::PUSHED_JOIN_REF: {
    } break;
    case AccessPath::FULL_TEXT_SEARCH: {
    } break;
    case AccessPath::CONST_TABLE: {
    } break;
    case AccessPath::MRR: {
    } break;
    case AccessPath::FOLLOW_TAIL: {
    } break;
    case AccessPath::INDEX_RANGE_SCAN: {
    } break;
    case AccessPath::INDEX_MERGE: {
    } break;
    case AccessPath::ROWID_INTERSECTION: {
    } break;
    case AccessPath::ROWID_UNION: {
    } break;
    case AccessPath::INDEX_SKIP_SCAN: {
    } break;
    case AccessPath::GROUP_INDEX_SKIP_SCAN: {
    } break;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
    } break;

    // Basic access paths that don't correspond to a specific table.
    case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
    } break;
    case AccessPath::FAKE_SINGLE_ROW: {
    } break;
    case AccessPath::ZERO_ROWS: {
    } break;
    case AccessPath::ZERO_ROWS_AGGREGATED: {
    } break;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
    } break;
    case AccessPath::UNQUALIFIED_COUNT: {
    } break;

    // Joins.
    case AccessPath::NESTED_LOOP_JOIN: {
    } break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
    } break;
    case AccessPath::BKA_JOIN: {
    } break;
    case AccessPath::HASH_JOIN: {
    } break;

    // Composite access paths.
    case AccessPath::FILTER: {
    } break;
    case AccessPath::SORT: {
    } break;
    case AccessPath::AGGREGATE: {
    } break;
    case AccessPath::TEMPTABLE_AGGREGATE: {
    } break;
    case AccessPath::LIMIT_OFFSET: {
    } break;
    case AccessPath::STREAM: {
    } break;
    case AccessPath::MATERIALIZE: {
    } break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
    } break;
    case AccessPath::APPEND: {
    } break;
    case AccessPath::WINDOW: {
    } break;
    case AccessPath::WEEDOUT: {
    } break;
    case AccessPath::REMOVE_DUPLICATES: {
    } break;
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
    } break;
    case AccessPath::ALTERNATIVE: {
    } break;
    case AccessPath::CACHE_INVALIDATOR: {
    } break;

    // Access paths that modify tables.
    case AccessPath::DELETE_ROWS: {
    } break;
    case AccessPath::UPDATE_ROWS: {
    } break;
    default:
      break;
  }

  return nullptr;
}

}  // namespace Optimizer
}  // namespace ShannonBase