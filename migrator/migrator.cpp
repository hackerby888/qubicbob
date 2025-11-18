#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>

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

        if (fromTick > toTick) {
            std::cerr << "Invalid range: from > to" << std::endl;
            return 1;
        }

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
        for (uint32_t tick = fromTick; tick <= toTick; ++tick) {
            (void)db_delete_tick_data(tick);
            (void)db_delete_tick_vote(tick);

            if ((tick - fromTick) % 10000 == 0) {
                Logger::get()->info("Unlinked up to tick {}", tick);
            }
        }
        Logger::get()->info("Section 1 complete.");

        // ---------------------------------------------
        // Section 2: vtick migration
        // - read vtick from KeyDB (db_get_vtick inside db_migrate_vtick)
        // - set the same key in Kvrocks
        // - unlink source key from KeyDB
        // ---------------------------------------------
        Logger::get()->info("Section 2: vtick migration KeyDB -> Kvrocks...");
        size_t migrated = 0;
        size_t txMigrated = 0;
        for (uint32_t tick = fromTick; tick <= toTick; ++tick) {
            // Read vtick content to access TickData and migrate transactions
            FullTickStruct fullTick;
            if (db_get_vtick(tick, fullTick)) {
                m256i zeroDigest(m256i::zero());
                for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; ++i) {
                    // Skip empty placeholders
                    if (fullTick.td.transactionDigests[i] != zeroDigest) {
                        std::string txHash = fullTick.td.transactionDigests[i].toQubicHash(); // required for canonical tx hash
                        if (db_migrate_transaction(txHash)) {
                            ++txMigrated;
                        }
                    }
                }
                bool vtickOk = db_migrate_vtick(tick);
                if (vtickOk) {
                    ++migrated;
                }
            }

            if ((tick - fromTick) % 10000 == 0) {
                Logger::get()->info("Migrated {} vtick entries and {} transactions. Latest tick={}", migrated, txMigrated, tick);
            }
        }
        Logger::get()->info("Section 2 complete. Migrated {} vtick entries and {} transactions.", migrated, txMigrated);

        // ---------------------------------------------
        // Section 3: log migration
        // - read tick_log_range from KeyDB to know the log range of that tick
        // - migrate all logs in that range to Kvrocks
        // - migrate all log_ranges for the tick to Kvrocks
        // ---------------------------------------------
        Logger::get()->info("Section 3: log migration KeyDB -> Kvrocks...");
        size_t logsMigrated = 0;
        size_t rangesMigrated = 0;

        for (uint32_t tick = fromTick; tick <= toTick; ++tick) {
            long long fromLogId = -1;
            long long length = -1;

            // Read the aggregated per-tick log range
            if (!db_get_log_range_for_tick(tick, fromLogId, length)) {
                // Could not read range for this tick; continue to next tick
                continue;
            }

            // Migrate per-tick/per-tx log range artifacts regardless of whether logs exist
            if (db_migrate_log_ranges(tick)) {
                ++rangesMigrated;
            }

            // If the tick has logs, migrate each log by id
            if (fromLogId != -1 && length > 0) {
                const uint64_t startId = static_cast<uint64_t>(fromLogId);
                const uint64_t endId   = static_cast<uint64_t>(fromLogId + length - 1);
                for (uint64_t logId = startId; logId <= endId; ++logId) {
                    if (db_migrate_log(epoch, logId)) {
                        ++logsMigrated;
                    }
                }
            }

            if ((tick - fromTick) % 10000 == 0) {
                Logger::get()->info("Section 3 progress: migrated {} logs and {} log_range entries. Latest tick={}", logsMigrated, rangesMigrated, tick);
            }
        }
        Logger::get()->info("Section 3 complete. Migrated {} logs and {} log_range entries.", logsMigrated, rangesMigrated);

        db_close();
        Logger::get()->info("Migration finished successfully.");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 99;
    }
}