#include "database/db.h"
// Compress a verified tick: pack TickData + up to 676 TickVotes into FullTickStruct,
// store via db_insert_vtick
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

    Logger::get()->trace("compressTick: Compressed tick {}", tick);
}

bool cleanRawTick(uint32_t fromTick, uint32_t toTick)
{
    Logger::get()->trace("Start cleaning raw tick data from {} to {}", fromTick, toTick);
    for (uint32_t tick = fromTick; tick <= toTick; tick++)
    {
        // Delete raw TickData
        if (!db_delete_tick_data(tick))
        {
            Logger::get()->warn("cleanRawTick: Failed to delete TickData for tick {}", tick);
        }

        // Delete all TickVotes for this tick (attempt all indices; API treats missing as success)
        db_delete_tick_vote(tick);
    }
    Logger::get()->trace("Cleaned raw tick data from {} to {}", fromTick, toTick);
    return true;
}