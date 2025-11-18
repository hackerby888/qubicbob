#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <thread>
#include <atomic>

#include "cxxopts.hpp"
#include "Logger.h"

#include "database/db.h"       // for db_connect/db_get_vtick/db_delete_tick_* and FullTickStruct/TickData/TickVote
#include "zstd.h"     // compression for vtick re-serialization

// ... existing code ...
int main(int argc, char** argv) {
    try {
        cxxopts::Options options("migrator", "Migrate data from KeyDB to Kvrocks without SCAN.");
        options.add_options()
                ("keydb", "KeyDB URI (source)", cxxopts::value<std::string>())
                ("kvrocks", "Kvrocks URI (destination)", cxxopts::value<std::string>())
                ("epoch", "Epoch to migrate (for bookkeeping/logging)", cxxopts::value<uint16_t>())
                ("from", "From tick (inclusive)", cxxopts::value<uint32_t>())
                ("to", "To tick (inclusive)", cxxopts::value<uint32_t>())
                ("l,log-level", "Log level (trace|debug|info|warn|error|critical)", cxxopts::value<std::string>()->default_value("info"))
                ("h,help", "Show help");

        auto result = options.parse(argc, argv);
        if (result.count("help") || !result.count("keydb") || !result.count("kvrocks") ||
            !result.count("epoch") || !result.count("from") || !result.count("to")) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        const std::string keydbUri  = result["keydb"].as<std::string>();
        std::string kvrocksUri      = result["kvrocks"].as<std::string>();
        const uint16_t epoch        = result["epoch"].as<uint16_t>();
        const uint32_t fromTick     = result["from"].as<uint32_t>();
        const uint32_t toTick       = result["to"].as<uint32_t>();
        const std::string logLevel  = result["log-level"].as<std::string>();

        if (fromTick > toTick) {
            std::cerr << "Invalid range: from > to" << std::endl;
            return 1;
        }

        Logger::init(logLevel);

        if (kvrocksUri.find('?') == std::string::npos) {
            kvrocksUri += "?pool_size=16";
        } else {
            kvrocksUri += "&pool_size=16";
        }

        // Source (KeyDB)
        db_connect(keydbUri);
        // Destination (Kvrocks)
        db_kvrocks_connect(kvrocksUri);

        Logger::get()->info("Starting migration. epoch={}, range=[{}, {}]", epoch, fromTick, toTick);

        // ---------------------------------------------
        // Section 1: tickData and tickVote unlink on KeyDB
        // ---------------------------------------------
        Logger::get()->info("Section 1: Unlink tick_data and tick_vote on KeyDB...");
        {
            const unsigned int kThreads = 4;
            const uint32_t kBatchSize = 1000; // each batch call deletes up to 1000 ticks
            std::atomic<uint32_t> nextTick(fromTick);
            std::vector<std::thread> workers;
            workers.reserve(kThreads);

            for (unsigned int i = 0; i < kThreads; ++i) {
                workers.emplace_back([&]() {
                    while (true) {
                        // Fetch the next batch start
                        uint32_t start = nextTick.fetch_add(kBatchSize, std::memory_order_relaxed);
                        if (start > toTick) break;

                        // Cap the batch to the remaining ticks
                        uint32_t count = std::min<uint32_t>(kBatchSize, toTick - start + 1);

                        // Batch delete tick_data and tick_vote for [start, start+count-1]
                        if (db_delete_tick_data_batch(start, count))
                        {
                            Logger::get()->info("Unlinked tickData from {} to {}", start, start + count);
                        }
                        if (db_delete_tick_vote_batch(start, count))
                        {
                            Logger::get()->info("Unlinked tickVote from {} to {}", start, start + count);
                        }
                    }
                });
            }

            for (auto &t : workers) t.join();
        }
        Logger::get()->info("Section 1 complete.");

        // ---------------------------------------------
        // Section 2: vtick migration
        // - read vtick from KeyDB (db_get_vtick inside db_migrate_vtick)
        // - set the same key in Kvrocks
        // - unlink source key from KeyDB
        // ---------------------------------------------
        Logger::get()->info("Section 2: vtick migration KeyDB -> Kvrocks...");
        // ---------------------------------------------
        // Section 2: vtick migration
        // Parallelized with 16 threads
        // ---------------------------------------------
        {
            const unsigned int kThreads = 16;
            std::atomic<uint32_t> nextTick(fromTick);
            std::atomic<size_t> migrated{0};
            std::atomic<size_t> txMigrated{0};

            std::vector<std::thread> workers;
            workers.reserve(kThreads);

            for (unsigned int i = 0; i < kThreads; ++i) {
                workers.emplace_back([&]() {
                    while (true) {
                        uint32_t tick = nextTick.fetch_add(1, std::memory_order_relaxed);
                        if (tick > toTick) break;

                        // Read vtick content to access TickData and migrate transactions
                        FullTickStruct fullTick;
                        if (db_get_vtick(tick, fullTick)) {
                            m256i zeroDigest(m256i::zero());
                            size_t localTxMigrated = 0;

                            for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; ++i) {
                                // Skip empty placeholders
                                if (fullTick.td.transactionDigests[i] != zeroDigest) {
                                    std::string txHash = fullTick.td.transactionDigests[i].toQubicHash(); // required for canonical tx hash
                                    if (db_migrate_transaction(txHash)) {
                                        ++localTxMigrated;
                                    }
                                }
                            }

                            if (localTxMigrated) {
                                txMigrated.fetch_add(localTxMigrated, std::memory_order_relaxed);
                            }

                            bool vtickOk = db_migrate_vtick(tick);
                            if (vtickOk) {
                                migrated.fetch_add(1, std::memory_order_relaxed);
                            }
                        }

                        if ((tick - fromTick) % 10000 == 0) {
                            Logger::get()->info(
                                "Migrated {} vtick entries and {} transactions. Latest tick={}",
                                migrated.load(std::memory_order_relaxed),
                                txMigrated.load(std::memory_order_relaxed),
                                tick
                            );
                        }
                    }
                });
            }

            for (auto &t : workers) t.join();

            Logger::get()->info(
                "Section 2 complete. Migrated {} vtick entries and {} transactions.",
                migrated.load(),
                txMigrated.load()
            );
        }
        // ---------------------------------------------
        // Section 3: log migration KeyDB -> Kvrocks...
        // - read tick_log_range from KeyDB to know the log range of that tick
        // - migrate all logs in that range to Kvrocks
        // - migrate all log_ranges for the tick to Kvrocks
        // ---------------------------------------------
        Logger::get()->info("Section 3: log migration KeyDB -> Kvrocks...");
        // ---------------------------------------------
        // Section 3: log migration
        // Parallelized with 16 threads
        // ---------------------------------------------
        {
            const unsigned int kThreads = 16;
            std::atomic<uint32_t> nextTick(fromTick);
            std::atomic<size_t> logsMigrated{0};
            std::atomic<size_t> rangesMigrated{0};

            std::vector<std::thread> workers;
            workers.reserve(kThreads);

            for (unsigned int i = 0; i < kThreads; ++i) {
                workers.emplace_back([&]() {
                    while (true) {
                        uint32_t tick = nextTick.fetch_add(1, std::memory_order_relaxed);
                        if (tick > toTick) break;

                        long long fromLogId = -1;
                        long long length = -1;

                        // Read the aggregated per-tick log range
                        if (!db_get_log_range_for_tick(tick, fromLogId, length)) {
                            // Could not read range for this tick; continue to next tick
                            continue;
                        }

                        // Migrate per-tick/per-tx log range artifacts regardless of whether logs exist
                        if (db_migrate_log_ranges(tick)) {
                            rangesMigrated.fetch_add(1, std::memory_order_relaxed);
                        }

                        // If the tick has logs, migrate each log by id
                        if (fromLogId != -1 && length > 0) {
                            const uint64_t startId = static_cast<uint64_t>(fromLogId);
                            const uint64_t endId   = static_cast<uint64_t>(fromLogId + length - 1);
                            size_t localLogs = 0;
                            for (uint64_t logId = startId; logId <= endId; ++logId) {
                                if (db_migrate_log(epoch, logId)) {
                                    ++localLogs;
                                }
                            }
                            if (localLogs) {
                                logsMigrated.fetch_add(localLogs, std::memory_order_relaxed);
                            }
                        }

                        if ((tick - fromTick) % 10000 == 0) {
                            Logger::get()->info(
                                "Section 3 progress: migrated {} logs and {} log_range entries. Latest tick={}",
                                logsMigrated.load(std::memory_order_relaxed),
                                rangesMigrated.load(std::memory_order_relaxed),
                                tick
                            );
                        }
                    }
                });
            }

            for (auto &t : workers) t.join();

            Logger::get()->info(
                "Section 3 complete. Migrated {} logs and {} log_range entries.",
                logsMigrated.load(),
                rangesMigrated.load()
            );
        }

        db_close();
        Logger::get()->info("Migration finished successfully.");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 99;
    }
}