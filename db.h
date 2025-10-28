/*
 db.h — Redis persistence API for node data

 Overview
 - Provides a narrow, implementation-agnostic interface for persisting and retrieving
   TickVotes, TickData, transactions, and log events.
 - Encapsulates connection lifecycle management to Redis.

 Keyspace conventions (conceptual)
 - tick_vote:{tick}:{computorIndex}:{hash}        -> binary TickVote
 - tick_data:{tick}:{computorIndex}:{hash}        -> binary TickData
 - transaction:{tick}:{hash}                      -> binary Transaction (or envelope)
 - log:{epoch}:{tick}:{txHash}:{type}:{logId}:{hash} -> binary log content
 - log_range:{tick}:{txId}                        -> from/length pair (per-tx in a tick)
 - db_status                                      -> latest overall tick/epoch, latest event tick/epoch
 - db_status:epoch:{epoch}                        -> per-epoch fields such as latest_log_id, latest_verified_tick

 Binary layout and endianness
 - All structs are written and read as-is (host byte order, little-endian on typical targets). Consumers must
   run on consistent architectures or serialize/deserialize explicitly when crossing boundaries.
 - LogEvent is stored/handled as a packed byte vector with a fixed-size header; see the LogEvent docs below.

 Error handling and return values
 - Functions returning bool: true means success; false means the operation failed or the requested entity was absent.
 - Functions returning integral counts or IDs: negative values indicate error or absence as documented per function.
 - Connection functions may throw std::runtime_error on failures where indicated.

 Concurrency
 - The API is designed to be callable from multiple threads. Actual thread safety depends on the underlying
   Redis client usage in the implementation. If the underlying client is not thread-safe, the implementation
   must provide appropriate synchronization.

 Atomicity
 - Update helpers that state “atomically” must ensure single-writer semantics via Redis-side primitives
   (e.g., Lua scripts, transactions, or WATCH/MULTI/EXEC) so that monotonicity constraints are honored.
*/

#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <immintrin.h> // For m256i
#include "structs.h"
#include "Logger.h"
#include "LogEvent.h"
// Forward declaration for the Redis client
namespace sw { namespace redis { class Redis; }}

// Placeholder definitions for constants from structs.h
#define SIGNATURE_SIZE 64
#define NUMBER_OF_TRANSACTIONS_PER_TICK 1024
#define MAX_NUMBER_OF_CONTRACTS 1024

// ---- Data Structures to be Stored ----

struct TickVote
{
    unsigned short computorIndex;
    unsigned short epoch;
    unsigned int tick;

    unsigned short millisecond;
    unsigned char second;
    unsigned char minute;
    unsigned char hour;
    unsigned char day;
    unsigned char month;
    unsigned char year;

    unsigned int prevResourceTestingDigest;
    unsigned int saltedResourceTestingDigest;

    unsigned int prevTransactionBodyDigest;
    unsigned int saltedTransactionBodyDigest;

    m256i prevSpectrumDigest;
    m256i prevUniverseDigest;
    m256i prevComputerDigest;
    m256i saltedSpectrumDigest;
    m256i saltedUniverseDigest;
    m256i saltedComputerDigest;

    m256i transactionDigest;
    m256i expectedNextTickTransactionDigest;

    unsigned char signature[SIGNATURE_SIZE];
};

struct FullTickStruct
{
    TickData td;
    TickVote tv[676];
};

// ---- Database Interface ----

/**
 * Connects to the Redis server and prepares the DB layer for use.
 * - connectionString example: "tcp://127.0.0.1:6379"
 * Throws:
 * - std::runtime_error on connection or authentication failure.
 */
void db_connect(const std::string& connectionString);

/**
 * Closes the Redis connection and releases any associated resources.
 * Safe to call multiple times.
 */
void db_close();

// ---- Insertion Functions ----

/**
 * Inserts a TickVote as a binary blob.
 * Key format: tick_vote:{tick}:{computorIndex}:{hash}
 * Returns:
 * - true on success
 * - false if the write fails
 */
bool db_insert_tick_vote(const TickVote& vote);

/**
 * Inserts a TickData as a binary blob.
 * Key format: tick_data:{tick}:{computorIndex}:{hash}
 * Returns:
 * - true on success
 * - false on write failure
 */
bool db_insert_tick_data(const TickData& data);

/**
 * Inserts a Transaction payload.
 * Key format: transaction:{tick}:{hash}
 * Returns:
 * - true on success
 * - false on write failure or invalid input
 */
bool db_insert_transaction(const Transaction* tx);

/**
 * Inserts a log event payload.
 * Expected content layout begins with a 26-byte packed header (see LogEvent).
 * Key format: log:{epoch}:{tick}:{txHash}:{type}:{logId}:{hash}
 * Returns:
 * - true on success
 * - false on write failure
 */
bool db_insert_log(uint16_t epoch, uint32_t tick, uint64_t logId, int logSize, const uint8_t* content);

/**
 * Inserts the per-tx log-id range for a given tick.
 * Key format: log_range:{tick}:{txId}
 * Returns:
 * - true on success
 * - false on write failure
 */
bool db_insert_log_range(uint32_t tick, const ResponseAllLogIdRangesFromTick& logRange);

/**
 * Atomically updates the latest tick and epoch in the global DB status if and only if
 * the provided tick is strictly greater than the stored value.
 * Returns:
 * - true if updated or if an equal/higher value is already stored
 * - false on failure
 */
bool db_update_latest_tick_and_epoch(uint32_t tick, uint16_t epoch);

// ---- Retrieval Functions ----

/**
 * Reads the latest tick and epoch from the global DB status.
 * Returns:
 * - true on success (outputs are set)
 * - false on failure or if not present
 */
bool db_get_latest_tick_and_epoch(uint32_t& tick, uint16_t& epoch);

/**
 * Atomically updates the latest logging event tick/epoch in DB status, only if the
 * new tick is strictly greater than the stored event tick.
 * Returns:
 * - true if updated or already up-to-date
 * - false on failure
 */
bool db_update_latest_event_tick_and_epoch(uint32_t tick, uint16_t epoch);

/**
 * Reads the latest logging event tick/epoch from DB status.
 * Returns:
 * - true on success (outputs are set)
 * - false on failure or if fields are absent
 */
bool db_get_latest_event_tick_and_epoch(uint32_t& tick, uint16_t& epoch);

/**
 * Update the latest log id for a specific epoch.
 * Stored at: db_status:epoch:{epoch} field latest_log_id
 * Returns true on success.
 */
bool db_update_latest_log_id(uint16_t epoch, long long logId);

/**
 * Get the latest log id for a specific epoch.
 * Reads: db_status:epoch:{epoch} field latest_log_id
 * Returns:
 * - >= 0 on success
 * - -1 on error or if the field is absent
 */
long long db_get_latest_log_id(uint16_t epoch);

// Track latest verified tick per epoch

/**
 * Update the latest verified tick for a specific epoch.
 * Stored at: db_status:epoch:{epoch} field latest_verified_tick
 * Only updates if the new tick > stored value.
 * Returns true on success.
 */
bool db_update_latest_verified_tick(uint32_t tick);

/**
 * Get the latest verified tick for a specific epoch.
 * Reads: db_status:epoch:{epoch} field latest_verified_tick
 * Returns:
 * - >= 0 on success
 * - -1 if not present or on error
 */
long long db_get_latest_verified_tick();

/**
 * Count the number of votes for a given tick.
 * Returns:
 * - >= 0 vote count
 * - -1 on error
 */
long long db_get_tick_vote_count(uint32_t tick);

/**
 * Retrieve a single TickVote for a given tick and computor index.
 * Returns:
 * - true if found (vote is populated)
 * - false if not found or on error
 */
bool db_get_tick_vote(uint32_t tick, uint16_t computorIndex, TickVote& vote);

/**
 * Retrieve all TickVotes for a given tick.
 * Returns:
 * - vector of votes (empty on failure or if none exist)
 */
std::vector<TickVote> db_get_tick_votes(uint32_t tick);

/**
 * Count the number of transactions for a specific tick.
 * Returns:
 * - >= 0 transaction count
 * - -1 on error
 */
long long db_get_tick_transaction_count(uint32_t tick);

/**
 * Retrieve all log events for a transaction hash.
 * Returns a vector of LogEvent; empty on failure or if none exist.
 */
std::vector<LogEvent> db_get_logs_by_tx_hash(const std::string& txHash);

/**
 * Retrieve log events within an epoch and tick range [start_tick, end_tick].
 * Returns a vector of LogEvent; empty on failure or if none exist.
 */
std::vector<LogEvent> db_get_logs_by_tick_range(uint16_t epoch, uint32_t start_tick, uint32_t end_tick);

/**
 * Retrieve TickData for a specific tick.
 * Implementations are expected to ensure consistency across redundant copies if stored.
 * Returns:
 * - true if found and consistent (data is populated)
 * - false otherwise
 */
bool db_get_tick_data(uint32_t tick, TickData& data);

/**
 * Retrieve the raw binary data of a transaction by hash.
 * Returns:
 * - true if found (tx_data populated)
 * - false if not found or on error
 */

bool db_check_log_range(uint32_t tick);
bool check_logid(uint64_t logId);
bool db_get_log_range_all_txs(uint32_t tick, ResponseAllLogIdRangesFromTick &logRange);
bool db_has_tick_data(uint32_t tick);
bool db_get_transaction(const std::string& tx_hash, std::vector<uint8_t>& tx_data);
bool db_check_transaction_exist(const std::string& tx_hash);

// ---- Deletion Functions ----
// Deletes TickData for a specific tick.
// Returns true if the key was removed (or did not exist), false on Redis error.
bool db_delete_tick_data(uint32_t tick);

// Deletes a TickVote for a specific tick and computor index.
// Returns true if the key was removed (or did not exist), false on Redis error.
bool db_delete_tick_vote(uint32_t tick, uint16_t computorIndex);
bool db_delete_tick_vote(uint32_t tick);

// New: get log range for a specific tx in a tick
// Returns true on success; outputs fromLogId and length.
bool db_get_log_range_for_tx(uint32_t tick, uint32_t txId, long long& fromLogId, long long& length);

// New: get aggregated log range for the whole tick (from key "...:-1")
// Returns true on success; outputs fromLogId and length.
bool db_get_log_range_for_tick(uint32_t tick, long long& fromLogId, long long& length);

// Digest helpers (per-tick materialized digests). Return zeroed m256i on absence/failure.
m256i db_getSpectrumDigest(uint32_t tick);
m256i db_getUniverseDigest(uint32_t tick);

// Store and get Computors by epoch. Key: "computor:<epoch>"
bool db_insert_computors(const Computors& comps);
bool db_get_computors(uint16_t epoch, Computors& comps);
bool db_log_exists(uint16_t epoch, uint64_t logId);

bool db_insert_vtick(uint32_t tick, const FullTickStruct& fullTick);
bool db_get_vtick(uint32_t tick, FullTickStruct& outFullTick);

bool db_get_log(uint16_t epoch, uint64_t logId, LogEvent &log);

long long db_get_last_indexed_tick();
bool db_update_last_indexed_tick(uint32_t tick);

// Store per-transaction index info for fast lookup by tx-hash.
// Key is expected to be "itx:<txHash>".
// Fields stored:
//   - tx_index     (int)       : index within the tick (0..NUMBER_OF_TRANSACTIONS_PER_TICK-1)
//   - from_log_id  (long long) : first logId for this tx in the tick, or -1 if none
//   - to_log_id    (long long) : last  logId for this tx in the tick, or -1 if none
//   - executed     (0/1)       : whether the tx was executed (best-effort heuristic)
bool db_set_indexed_tx(const char* key,
                       int tx_index,
                       long long from_log_id,
                       long long to_log_id,
                       bool executed);

bool db_get_indexed_tx(const char* tx_hash,
                       int& tx_index,
                       long long& from_log_id,
                       long long& to_log_id,
                       bool& executed);


bool db_add_indexer(const std::string &key, uint32_t tickNumber);

bool db_get_combined_log_range_for_ticks(uint32_t startTick, uint32_t endTick, long long &fromLogId, long long &length);

std::vector<TickVote> db_try_to_get_votes(uint32_t tick);

std::vector<uint32_t> db_search_log(uint32_t scIndex, uint32_t scLogType, uint32_t fromTick, uint32_t toTick,
                                    std::string topic1, std::string topic2, std::string topic3);