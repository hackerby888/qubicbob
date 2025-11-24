#include "database/db.h"
#include "shim.h"
bool cleanTransactionAndLogsAndSaveToDisk(TickData& td, ResponseAllLogIdRangesFromTick& lr)
{
    for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
    {
        if (td.transactionDigests[i] != m256i::zero())
        {
            db_copy_transaction_to_kvrocks(td.transactionDigests[i].toQubicHash());
            if (lr.fromLogId[i] > 0 && lr.length[i] > 0)
            {
                long long start = lr.fromLogId[i];
                long long end = start + lr.length[i] - 1; // inclusive
                if (db_move_logs_to_kvrocks_by_range(td.epoch, start, end))
                {
                    db_delete_logs(td.epoch, start, end);
                }
            }
            db_delete_transaction(td.transactionDigests[i].toQubicHash());
        }
    }
}
void compressTickAndMoveToKVRocks(uint32_t tick)
{
    // Load TickData
    // Prepare the aggregated struct
    FullTickStruct full{};
    std::memset((void*)&full, 0, sizeof(full));
    db_get_tick_data(tick, full.td);
    auto votes = db_get_tick_votes(tick);
    for (const auto& v : votes)
    {
        if (v.computorIndex < 676 && v.epoch != 0)
        {
            std::memcpy((void*)&full.tv[v.computorIndex], &v, sizeof(TickVote));
        }
    }

    // Insert the compressed record
    if (!db_insert_vtick_to_kvrocks(tick, full))
    {
        Logger::get()->error("compressTick: Failed to insert vtick for tick {}", tick);
        return;
    }
    ResponseAllLogIdRangesFromTick lr{};
    if (db_get_log_ranges(tick, lr))
    {
        db_insert_cLogRange_to_kvrocks(tick, lr);
    }
    Logger::get()->trace("compressTick: Compressed tick {}", tick);
}

bool cleanTransactionLogs(uint32_t tick)
{
    TickData td{};
    ResponseAllLogIdRangesFromTick lr{};
    db_try_get_tick_data(tick, td);
    db_try_get_log_ranges(tick, lr);
    cleanTransactionAndLogsAndSaveToDisk(td, lr);
}

bool cleanRawTick(uint32_t fromTick, uint32_t toTick, bool withTransactions)
{
    Logger::get()->trace("Start cleaning raw tick data from {} to {}", fromTick, toTick);
    for (uint32_t tick = fromTick; tick <= toTick; tick++)
    {
        if (withTransactions)
        {
            cleanTransactionLogs(tick);
        }
        // Delete raw TickData
        if (!db_delete_tick_data(tick))
        {
            Logger::get()->warn("cleanRawTick: Failed to delete TickData for tick {}", tick);
        }

        // Delete all TickVotes for this tick (attempt all indices; API treats missing as success)
        db_delete_tick_vote(tick);
        db_delete_log_ranges(tick);
    }
    Logger::get()->trace("Cleaned raw tick data from {} to {}", fromTick, toTick);
    return true;
}

void garbageCleaner(std::atomic_bool& stopFlag)
{
    Logger::get()->info("Start garbage cleaner");
    long long lastCleanTickData = gCurrentFetchingTick - 1;
    long long lastCleanTransactionTick = gCurrentFetchingTick - 1;
    uint32_t lastReportedTick = 0;
    while (!stopFlag.load())
    {
        SLEEP(100);
        if (stopFlag.load()) break;
        if (gTickStorageMode == TickStorageMode::LastNTick)
        {
            long long cleanToTick = (long long)(gCurrentIndexingTick.load()) - 5;
            cleanToTick = std::min(cleanToTick, (long long)(gCurrentIndexingTick) - 1 - gLastNTickStorage);
            if (lastCleanTickData < cleanToTick)
            {
                if (cleanRawTick(lastCleanTickData + 1, cleanToTick, gTxStorageMode == TxStorageMode::LastNTick /*also clean txs*/))
                {
                    lastCleanTickData = cleanToTick;
                }

                if (cleanToTick - lastReportedTick > 1000)
                {
                    Logger::get()->trace("Cleaned up to tick {}", cleanToTick);
                    lastReportedTick = cleanToTick;
                }
            }
        }
        else if (gTickStorageMode == TickStorageMode::Kvrocks)
        {
            long long cleanToTick = (long long)(gCurrentIndexingTick.load()) - 5;
            if (lastCleanTickData < cleanToTick)
            {
                for (long long t = lastCleanTickData + 1; t <= cleanToTick; t++)
                {
                    compressTickAndMoveToKVRocks(t);
                }
                Logger::get()->trace("Compressed tick {}->{} to kvrocks", lastCleanTickData + 1, cleanToTick);
                if (cleanRawTick(lastCleanTickData + 1, cleanToTick, false /*do not clean txs instantly*/))
                {
                    lastCleanTickData = cleanToTick;
                }
                Logger::get()->trace("Cleaned tick {}->{} in keydb", lastCleanTickData + 1, cleanToTick);
                if (cleanToTick - lastReportedTick > 1000)
                {
                    Logger::get()->trace("Compressed and cleaned up to tick {}", cleanToTick);
                    lastReportedTick = cleanToTick;
                }
            }
        }

        if (gTxStorageMode == TxStorageMode::Kvrocks)
        {
            long long cleanToTick = (long long)(gCurrentIndexingTick.load()) - 5;
            cleanToTick = std::min(cleanToTick, (long long)(gCurrentIndexingTick) - 1 - gTxTickToLive);
            if (lastCleanTransactionTick < cleanToTick)
            {
                for (long long t = lastCleanTransactionTick + 1; t <= cleanToTick; t++)
                {
                    cleanTransactionLogs(t);
                }
                lastCleanTransactionTick = cleanToTick;
            }
        }
    }
    if (gIsEndEpoch)
    {
        Logger::get()->info("Garbage cleaner detected END EPOCH signal. Cleaning all data left on RAM");
        if (gTickStorageMode == TickStorageMode::LastNTick)
        {
            long long cleanToTick = (long long)(gCurrentIndexingTick.load()) - 1;
            if (lastCleanTickData < cleanToTick)
            {
                if (cleanRawTick(lastCleanTickData + 1, cleanToTick, true))
                {
                    Logger::get()->info("Cleaned all raw tick data");
                }
            }
        }
        else if (gTickStorageMode == TickStorageMode::Kvrocks)
        {
            long long cleanToTick = (long long)(gCurrentIndexingTick.load()) - 1;
            if (lastCleanTickData < cleanToTick)
            {
                for (long long t = lastCleanTickData + 1; t <= cleanToTick; t++)
                {
                    compressTickAndMoveToKVRocks(t);
                }
                Logger::get()->trace("Compressed tick {}->{} to kvrocks", lastCleanTickData + 1, cleanToTick);
                if (cleanRawTick(lastCleanTickData + 1, cleanToTick, true))
                {
                    Logger::get()->info("Cleaned all raw tick data");
                }
            }
        }
    }
    Logger::get()->info("Exited garbage cleaner");
}