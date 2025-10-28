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
*/
#ifndef __SHANNONBASE_TRANSACTION_H__
#define __SHANNONBASE_TRANSACTION_H__
#include <chrono>
#include <shared_mutex>
#include "sql/current_thd.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/utils.h"
class THD;
class trx_t;
class ReadView;

namespace ShannonBase {

/**This class is used for an interface of real implementation of trans.
Here, is used as an interface of innobase transaction. In future we can
use any transaction impl to replace innobase's trx used here.
*/

class Transaction : public MemoryObject {
 public:
  Transaction(THD *thd = current_thd);
  virtual ~Transaction();

  // here, we use innodb's trx_id_t as ours. the defined in innodb is: typedef ib_id_t trx_id_t;
  using ID = uint64_t;
  static constexpr ID MAX_ID = std::numeric_limits<uint64_t>::max();
  // same order with trx_t::isolation_level_t::
  enum class ISOLATION_LEVEL : uint8 { READ_UNCOMMITTED, READ_COMMITTED, READ_REPEATABLE, SERIALIZABLE };
  enum class STATUS : uint8 { NOT_START, ACTIVE, PREPARED, COMMITTED_IN_MEMORY };

  // gets the existed trx or create a new one if not existed.
  static Transaction *get_or_create_trx(THD *);

  // gets the trx from thd.
  static Transaction *get_trx_from_thd(THD *const thd);

  static ShannonBase::Transaction::ISOLATION_LEVEL get_rpd_isolation_level(THD *thd);

  static void free_trx_from_thd(THD *const thd);

  void set_trx_on_thd(THD *const thd);

  void reset_trx_on_thd(THD *const thd);

  virtual void set_isolation_level(ISOLATION_LEVEL level) { m_iso_level = level; }

  virtual ISOLATION_LEVEL isolation_level() const { return m_iso_level; }

  virtual int begin(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);

  virtual int commit();

  virtual int rollback();

  virtual int begin_stmt(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);

  virtual int rollback_stmt();

  virtual int rollback_to_savepoint(void *const savepoint);

  virtual void set_read_only(bool read_only);

  virtual ::ReadView *acquire_snapshot();

  virtual int release_snapshot();

  virtual bool has_snapshot() const;

  virtual bool is_auto_commit();

  virtual bool is_active();

  virtual bool changes_visible(Transaction::ID trx_id, const char *table_name);

  virtual Transaction::ID get_id();

  virtual ::ReadView *get_snapshot() const;

 private:
  THD *m_thd;

  // read only trx.
  bool m_read_only{false};

  /**here, we use innodb's trx as ours. in future, we will impl rpl own
   * transaction. But, now that, we use innodb's.*/
  trx_t *m_trx_impl{nullptr};

  ISOLATION_LEVEL m_iso_level{ISOLATION_LEVEL::READ_REPEATABLE};

  bool m_stmt_active{false};
};

class TransactionGuard {
 public:
  TransactionGuard(Transaction *trx) : m_trx(trx) {}
  ~TransactionGuard() {
    if (m_trx && m_trx->is_active()) m_trx->rollback();
  }
  void commit() {
    m_trx->commit();
    m_trx = nullptr;
  }

 private:
  Transaction *m_trx;
};

class TransactionJournal {
 public:
  // Status Enum
  enum Entry_Status {
    ACTIVE = 0,     // Active (uncommitted)
    COMMITTED = 1,  // Committed
    ABORTED = 2     // Rolled back
  };

  TransactionJournal(size_t capacity) : m_capacity(capacity) {}

  virtual ~TransactionJournal() { clear(); }

  TransactionJournal(TransactionJournal &&other) noexcept
      : m_capacity(other.m_capacity),
        m_entry_count(other.m_entry_count.load()),
        m_total_size(other.m_total_size.load()) {
    std::unique_lock lock1(m_mutex, std::defer_lock);
    std::unique_lock lock2(other.m_mutex, std::defer_lock);
    std::lock(lock1, lock2);

    m_entries = std::move(other.m_entries);
    m_txn_entries = std::move(other.m_txn_entries);
    m_active_txns = std::move(other.m_active_txns);

    other.m_entry_count.store(0);
    other.m_total_size.store(0);
  }

  TransactionJournal &operator=(TransactionJournal &&other) noexcept {
    if (this != &other) {
      std::unique_lock lock1(m_mutex, std::defer_lock);
      std::unique_lock lock2(other.m_mutex, std::defer_lock);
      std::lock(lock1, lock2);

      m_capacity = other.m_capacity;
      m_entries = std::move(other.m_entries);
      m_txn_entries = std::move(other.m_txn_entries);
      m_active_txns = std::move(other.m_active_txns);

      m_entry_count.store(other.m_entry_count.load());
      m_total_size.store(other.m_total_size.load());

      other.m_entry_count.store(0);
      other.m_total_size.store(0);
    }
    return *this;
  }

  TransactionJournal(const TransactionJournal &) = delete;
  TransactionJournal &operator=(const TransactionJournal &) = delete;

  // Log Entry
  struct Entry {
    // Basic Information
    row_id_t row_id : 20;   // Local row ID (supports 1M rows)
    uint8_t operation : 2;  // INSERT/UPDATE/DELETE
    uint8_t status : 2;     // ACTIVE/COMMITTED/ABORTED
    uint32_t reserved : 8;

    // Transaction Information
    Transaction::ID txn_id;                           // Transaction ID
    uint64_t scn;                                     // System Change Number (assigned at commit)
    std::chrono::system_clock::time_point timestamp;  // Timestamp

    // UPDATE Specific
    // Bitmap marking modified columns (256 columns, 32 bytes)
    std::bitset<MAX_COLUMNS> modified_columns;

    // Linked List Pointer
    Entry *prev;  // Points to previous version of the same row

    Entry() : row_id(0), operation(0), status(0), reserved(0), txn_id(0), scn(0), prev(nullptr) {}

    ~Entry() = default;
  };

  // Log Operations
  /**
   * Add log entry
   * @param entry: Log entry (move semantics)
   */
  void add_entry(Entry &&entry);

  /**
   * Commit transaction
   * @param txn_id: Transaction ID
   * @param commit_scn: Commit SCN
   */
  void commit_transaction(Transaction::ID txn_id, uint64_t commit_scn);

  /**
   * Abort transaction
   * @param txn_id: Transaction ID
   */
  void abort_transaction(Transaction::ID txn_id);

  // Visibility Checking
  /**
   * Check if row is visible to reader
   * @param row_id: Local row ID
   * @param reader_txn_id: Reader transaction ID
   * @param reader_scn: Reader snapshot SCN
   * @return: Returns true if visible
   */
  bool is_row_visible(row_id_t row_id, Transaction::ID reader_txn_id, uint64_t reader_scn) const;

  /**
   * Batch visibility check (vectorized)
   * @param start_row: Starting row
   * @param count: Number of rows
   * @param reader_txn_id: Reader transaction ID
   * @param reader_scn: Reader SCN
   * @param visibility_mask: Output bitmap (1 indicates visible)
   */
  void check_visibility_batch(row_id_t start_row, size_t count, Transaction::ID reader_txn_id, uint64_t reader_scn,
                              bit_array_t &visibility_mask) const;

  /**
   * Get row state at specified SCN
   * @param row_id: Local row ID
   * @param target_scn: Target SCN
   * @param modified_columns: Output modified columns (UPDATE operation)
   * @return: Operation type
   */
  ShannonBase::OPER_TYPE get_row_state_at_scn(row_id_t row_id, uint64_t target_scn,
                                              std::bitset<MAX_COLUMNS> *modified_columns = nullptr) const;

  /**
   * Clean up old versions
   * @param min_active_scn: Minimum active SCN (versions before this SCN can be cleaned)
   * @return: Number of entries cleaned
   */
  size_t purge(uint64_t min_active_scn);

  /**
   * Clean up aborted transactions
   */
  size_t purge_aborted();

  /**
   * Clear all logs
   */
  inline void clear() {
    std::unique_lock lock(m_mutex);

    m_entries.clear();
    m_txn_entries.clear();
    m_active_txns.clear();

    m_entry_count.store(0);
    m_total_size.store(0);
  }

  inline size_t get_entry_count() const { return m_entry_count.load(); }

  inline size_t get_total_size() const { return m_total_size.load(); }

  inline size_t get_active_txn_count() const {
    std::shared_lock lock(m_mutex);
    return m_active_txns.size();
  }

  /**
   * Print log content (for debugging)
   */
  void dump(std::ostream &out) const;

 private:
  // Configuration
  size_t m_capacity;  // IMCU capacity

  // Log entries indexed by row
  // key: local_row_id, value: version chain head (newest -> oldest)
  std::unordered_map<row_id_t, std::unique_ptr<Entry>> m_entries;

  // Indexed by transaction ID (for rollback)
  std::unordered_map<Transaction::ID, std::vector<Entry *>> m_txn_entries;

  // Active transaction set
  std::unordered_set<Transaction::ID> m_active_txns;

  // Concurrency Control
  mutable std::shared_mutex m_mutex;

  // Statistics
  std::atomic<size_t> m_entry_count{0};
  std::atomic<size_t> m_total_size{0};
};

}  // namespace ShannonBase
#endif  //__SHANNONBASE_TRANSACTION_H__