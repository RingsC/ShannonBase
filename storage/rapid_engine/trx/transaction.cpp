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

   The fundmental code for imcs. for transaction.
   Now that, we use innodb trx as rapid's. But, in future, we will impl
   our own trx implementation, because we use innodb trx id in rapid
   for our visibility check.
*/
#include "storage/rapid_engine/trx/transaction.h"

#include "sql/sql_class.h"                        // THD
#include "storage/innobase/include/read0types.h"  //ReadView
#include "storage/innobase/include/trx0roll.h"    // rollback
#include "storage/innobase/include/trx0trx.h"     // trx_t
#include "storage/rapid_engine/include/rapid_context.h"

namespace ShannonBase {

// defined in ha_shannon_rapid.cc
extern handlerton *shannon_rapid_hton_ptr;

static ShannonBase::Rapid_ha_data *&get_ha_data_or_null(THD *const thd) {
  ShannonBase::Rapid_ha_data **ha_data =
      reinterpret_cast<ShannonBase::Rapid_ha_data **>(thd_ha_data(thd, ShannonBase::shannon_rapid_hton_ptr));
  return *ha_data;
}

static ShannonBase::Rapid_ha_data *&get_ha_data(THD *const thd) {
  auto *&ha_data = get_ha_data_or_null(thd);
  if (ha_data == nullptr) {
    ha_data = new ShannonBase::Rapid_ha_data();
  }
  return ha_data;
}

static void destroy_ha_data(THD *const thd) {
  ShannonBase::Rapid_ha_data *&ha_data = get_ha_data(thd);
  delete ha_data;
  ha_data = nullptr;
}

ShannonBase::Transaction::ISOLATION_LEVEL Transaction::get_rpd_isolation_level(THD *thd) {
  ulong const tx_isolation = thd_tx_isolation(thd);

  if (tx_isolation == ISO_READ_UNCOMMITTED) {
    return ShannonBase::Transaction::ISOLATION_LEVEL::READ_UNCOMMITTED;
  } else if (tx_isolation == ISO_READ_COMMITTED) {
    return ShannonBase::Transaction::ISOLATION_LEVEL::READ_COMMITTED;
  } else if (tx_isolation == ISO_REPEATABLE_READ) {
    return ShannonBase::Transaction::ISOLATION_LEVEL::READ_REPEATABLE;
  } else
    return ShannonBase::Transaction::ISOLATION_LEVEL::SERIALIZABLE;
}

Transaction::Transaction(THD *thd) {
  m_thd = thd;
  m_trx_impl = trx_allocate_for_mysql();  // check_trx_exists(thd);
  m_read_only = false;
}

Transaction::~Transaction() {
  release_snapshot();
  rollback();
  trx_free_for_mysql(m_trx_impl);
}

Transaction *Transaction::get_trx_from_thd(THD *const thd) { return get_ha_data(thd)->get_trx(); }

void Transaction::set_trx_on_thd(THD *const thd) { return get_ha_data(thd)->set_trx(this); }

void Transaction::reset_trx_on_thd(THD *const thd) {
  get_ha_data(thd)->set_trx(nullptr);
  return destroy_ha_data(thd);
}

Transaction *Transaction::get_or_create_trx(THD *thd) {
  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  if (trx == nullptr) {
    trx = new ShannonBase::Transaction(thd);
    trx->set_trx_on_thd(thd);
  }

  return trx;
}

void Transaction::free_trx_from_thd(THD *const thd) {
  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  if (trx) {
    trx->reset_trx_on_thd(thd);
    delete trx;
    trx = nullptr;
  }
}

int Transaction::begin(ISOLATION_LEVEL iso_level) {
  m_iso_level = iso_level;
  trx_t::isolation_level_t is = trx_t::isolation_level_t::REPEATABLE_READ;

  switch (iso_level) {
    case ISOLATION_LEVEL::READ_UNCOMMITTED:
      is = trx_t::isolation_level_t::READ_UNCOMMITTED;
      break;
    case ISOLATION_LEVEL::READ_COMMITTED:
      is = trx_t::isolation_level_t::READ_COMMITTED;
      break;
    case ISOLATION_LEVEL::READ_REPEATABLE:
      is = trx_t::isolation_level_t::REPEATABLE_READ;
      break;
    case ISOLATION_LEVEL::SERIALIZABLE:
      is = trx_t::isolation_level_t::SERIALIZABLE;
      break;
    default:
      break;
  }

  ut_a(m_trx_impl);
  ut_a(m_trx_impl->state.load() == TRX_STATE_NOT_STARTED);
  m_trx_impl->isolation_level = is;

  m_trx_impl->auto_commit =
      m_thd != nullptr && !thd_test_options(m_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN) && thd_is_query_block(m_thd);

  trx_start_if_not_started(m_trx_impl, (m_read_only ? false : true), UT_LOCATION_HERE);
  return SHANNON_SUCCESS;
}

int Transaction::begin_stmt(ISOLATION_LEVEL iso_level) {
  if (m_stmt_active) rollback_stmt();

  if (!is_active()) {
    int ret = begin(iso_level);
    if (ret != SHANNON_SUCCESS) return ret;
  }

  ut_a(m_trx_impl);
  ut_a(trx_is_started(m_trx_impl));

  m_stmt_active = true;

  m_trx_impl->op_info = "statement";
  trx_savept_t stmt_savepoint = trx_savept_take(m_trx_impl);
  m_trx_impl->last_sql_stat_start.least_undo_no = stmt_savepoint.least_undo_no;

  return SHANNON_SUCCESS;
}

int Transaction::commit() {
  dberr_t error{DB_SUCCESS};
  if (trx_is_started(m_trx_impl)) {
    error = trx_commit_for_mysql(m_trx_impl);
  }
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

int Transaction::rollback() {
  dberr_t error{DB_SUCCESS};
  if (trx_is_started(m_trx_impl)) {
    error = trx_rollback_for_mysql(m_trx_impl);
  }
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

int Transaction::rollback_stmt() {
  if (!m_stmt_active) return SHANNON_SUCCESS;

  dberr_t error = DB_SUCCESS;

  if (m_trx_impl && trx_is_started(m_trx_impl)) {
    error = trx_rollback_to_savepoint(m_trx_impl, nullptr);
    m_trx_impl->op_info = "";
  }

  m_stmt_active = false;

  if (error != DB_SUCCESS) return HA_ERR_GENERIC;

  return SHANNON_SUCCESS;
}

int Transaction::rollback_to_savepoint(void *const savepoint) { return SHANNON_SUCCESS; }

void Transaction::set_read_only(bool read_only) { m_read_only = read_only; }

::ReadView *Transaction::acquire_snapshot() {
  if (!MVCC::is_view_active(m_trx_impl->read_view) && (m_trx_impl->isolation_level > TRX_ISO_READ_UNCOMMITTED)) {
    trx_assign_read_view(m_trx_impl);
  }

  return m_trx_impl->read_view;
}

int Transaction::release_snapshot() {
  if (trx_sys->mvcc && m_trx_impl->read_view && MVCC::is_view_active(m_trx_impl->read_view)) {
    trx_sys->mvcc->view_close(m_trx_impl->read_view, false);
  }

  return SHANNON_SUCCESS;
}

::ReadView *Transaction::get_snapshot() const { return m_trx_impl->read_view; }

bool Transaction::has_snapshot() const { return MVCC::is_view_active(m_trx_impl->read_view); }

bool Transaction::is_auto_commit() { return m_trx_impl->auto_commit; }

bool Transaction::is_active() { return trx_is_started(m_trx_impl); }

bool Transaction::changes_visible(Transaction::ID trx_id, const char *table_name) {
  if (MVCC::is_view_active(m_trx_impl->read_view)) {
    table_name_t name;
    name.m_name = const_cast<char *>(table_name);
    return m_trx_impl->read_view->changes_visible(trx_id, name);
  }

  return false;
}

Transaction::ID Transaction::get_id() { return m_trx_impl->id; }

void TransactionJournal::add_entry(Entry &&entry) {
  std::unique_lock lock(m_mutex);

  row_id_t row_id = entry.row_id;

  // Create new entry
  auto new_entry = std::make_unique<Entry>(std::move(entry));

  // Link to version chain
  auto it = m_entries.find(row_id);
  if (it != m_entries.end()) {
    new_entry->prev = it->second.release();
    it->second = std::move(new_entry);
  } else {
    m_entries[row_id] = std::move(new_entry);
  }

  // Add to transaction index
  m_txn_entries[entry.txn_id].push_back(m_entries[row_id].get());

  // Mark transaction as active
  m_active_txns.insert(entry.txn_id);

  m_entry_count.fetch_add(1);
  m_total_size.fetch_add(sizeof(Entry));
}

void TransactionJournal::commit_transaction(Transaction::ID txn_id, uint64_t commit_scn) {
  std::unique_lock lock(m_mutex);

  auto it = m_txn_entries.find(txn_id);
  if (it == m_txn_entries.end()) return;

  // Update status and SCN for all entries
  for (Entry *entry : it->second) {
    entry->status = COMMITTED;
    entry->scn = commit_scn;
  }

  // Remove from active transaction set
  m_active_txns.erase(txn_id);
}

void TransactionJournal::abort_transaction(Transaction::ID txn_id) {
  std::unique_lock lock(m_mutex);

  auto it = m_txn_entries.find(txn_id);
  if (it == m_txn_entries.end()) return;

  // Mark all entries as aborted
  for (Entry *entry : it->second) {
    entry->status = ABORTED;
  }

  // Remove from active transaction set
  m_active_txns.erase(txn_id);

  // Clean up index
  m_txn_entries.erase(it);
}

bool TransactionJournal::is_row_visible(row_id_t row_id, Transaction::ID reader_txn_id, uint64_t reader_scn) const {
  std::shared_lock lock(m_mutex);

  auto it = m_entries.find(row_id);
  if (it == m_entries.end()) {
    // No history record, indicates initial data, visible
    return true;
  }

  Entry *entry = it->second.get();

  // Traverse version chain (from new to old)
  while (entry != nullptr) {
    // 1. If it's the reader's own transaction, visible
    if (entry->txn_id == reader_txn_id) {
      return static_cast<ShannonBase::OPER_TYPE>(entry->operation) != ShannonBase::OPER_TYPE::OPER_DELETE &&
             entry->status != ABORTED;
    }

    // 2. If transaction not committed, not visible
    if (entry->status == ACTIVE) {
      entry = entry->prev;
      continue;
    }

    // 3. If transaction aborted, not visible
    if (entry->status == ABORTED) {
      entry = entry->prev;
      continue;
    }

    // 4. If transaction committed
    if (entry->status == COMMITTED) {
      // 4.1 If commit SCN is after reader snapshot, not visible
      if (entry->scn > reader_scn) {
        entry = entry->prev;
        continue;
      }

      // 4.2 If commit SCN is before reader snapshot
      // Check operation type
      if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_INSERT) {
        return true;  // Insert visible
      } else if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_DELETE) {
        return false;  // Delete not visible
      } else if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_UPDATE) {
        return true;  // Update visible
      }
    }

    entry = entry->prev;
  }

  // No visible version found
  return false;
}

void TransactionJournal::check_visibility_batch(row_id_t start_row, size_t count, Transaction::ID reader_txn_id,
                                                uint64_t reader_scn, bit_array_t &visibility_mask) const {
  std::shared_lock lock(m_mutex);

  for (size_t i = 0; i < count; i++) {
    row_id_t row_id = start_row + i;
    bool visible = is_row_visible(row_id, reader_txn_id, reader_scn);

    if (visible) {
      Utils::Util::bit_array_set(&visibility_mask, i);
    } else {
      Utils::Util::bit_array_reset(&visibility_mask, i);
    }
  }
}

ShannonBase::OPER_TYPE TransactionJournal::get_row_state_at_scn(row_id_t row_id, uint64_t target_scn,
                                                                std::bitset<MAX_COLUMNS> *modified_columns) const {
  std::shared_lock lock(m_mutex);

  auto it = m_entries.find(row_id);
  if (it == m_entries.end()) {
    return ShannonBase::OPER_TYPE::OPER_NONE;
  }

  Entry *entry = it->second.get();

  while (entry != nullptr) {
    if (entry->status == COMMITTED && entry->scn <= target_scn) {
      if (modified_columns &&
          static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_UPDATE) {
        *modified_columns = entry->modified_columns;
      }
      return static_cast<ShannonBase::OPER_TYPE>(entry->operation);
    }
    entry = entry->prev;
  }

  return ShannonBase::OPER_TYPE::OPER_NONE;
}

size_t TransactionJournal::purge(uint64_t min_active_scn) {
  std::unique_lock lock(m_mutex);

  size_t purged = 0;

  for (auto it = m_entries.begin(); it != m_entries.end();) {
    Entry *head = it->second.get();
    Entry *current = head;
    Entry *prev_valid = nullptr;

    // Keep the latest visible version
    bool found_visible = false;

    while (current != nullptr) {
      // If version is before minimum active SCN, and not the latest visible version
      if (current->status == COMMITTED && current->scn < min_active_scn && found_visible) {
        // Can clean up
        Entry *to_delete = current;
        current = current->prev;

        if (prev_valid) {
          prev_valid->prev = current;
        }

        delete to_delete;
        purged++;
        m_entry_count.fetch_sub(1);
        m_total_size.fetch_sub(sizeof(Entry));
      } else {
        // Keep
        if (current->status == COMMITTED) {
          found_visible = true;
          prev_valid = current;
        }
        current = current->prev;
      }
    }

    // If entire version chain is cleaned
    if (head == nullptr || (head->prev == nullptr && head->status == ABORTED)) {
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }

  return purged;
}

size_t TransactionJournal::purge_aborted() {
  std::unique_lock lock(m_mutex);

  size_t purged = 0;

  for (auto it = m_entries.begin(); it != m_entries.end();) {
    Entry *head = it->second.get();

    if (head->status == ABORTED && head->prev == nullptr) {
      // Only one aborted version, can delete
      it = m_entries.erase(it);
      purged++;
      m_entry_count.fetch_sub(1);
      m_total_size.fetch_sub(sizeof(Entry));
    } else {
      ++it;
    }
  }

  return purged;
}

void TransactionJournal::dump(std::ostream &out) const {
  std::shared_lock lock(m_mutex);

  out << "Transaction Journal: " << m_entry_count.load() << " entries\n";

  for (const auto &[row_id, entry] : m_entries) {
    Entry *current = entry.get();
    out << "  Row " << row_id << ": ";

    while (current != nullptr) {
      out << "[txn=" << current->txn_id << " scn=" << current->scn << " op=" << static_cast<int>(current->operation)
          << " status=" << static_cast<int>(current->status) << "] -> ";
      current = current->prev;
    }

    out << "NULL\n";
  }
}
}  // namespace ShannonBase