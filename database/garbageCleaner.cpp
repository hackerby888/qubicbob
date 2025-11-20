#include "database/db.h"

bool cleanTransactionAndLogs(TickData& td, ResponseAllLogIdRangesFromTick& lr)
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
    db_try_get_TickData(tick, td);
    db_try_get_log_ranges(tick, lr);
    cleanTransactionAndLogs(td, lr);
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

