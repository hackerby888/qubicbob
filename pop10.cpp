#include "database/db.h"
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <start_tick> <end_tick> <epoch> [redis_address]" << std::endl;
        std::cerr << "Example: " << argv[0] << " 1000 2000 1 tcp://127.0.0.1:6379" << std::endl;
        return 1;
    }

    uint32_t startTick = std::stoul(argv[1]);
    uint32_t endTick = std::stoul(argv[2]);
    uint16_t epoch = static_cast<uint16_t>(std::stoul(argv[3]));
    std::string redisAddress = "tcp://127.0.0.1:6379";
    Logger::init("info");

    if (argc > 4) {
        redisAddress = argv[4];
    }

    std::cout << "Connecting to KeyDB at " << redisAddress << "..." << std::endl;
    try {
        db_connect(redisAddress);
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to database: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Deleting data from tick " << startTick << " to " << endTick << " (Epoch: " << epoch << ")" << std::endl;

    for (uint32_t tick = startTick; tick <= endTick; ++tick) {
        // 1. Delete Log Events
        // We must retrieve the log range *before* deleting the metadata so we know which log IDs to delete.
        long long fromLogId = -1;
        long long length = -1;

        if (db_try_get_log_range_for_tick(tick, fromLogId, length)) {
            if (fromLogId != -1 && length > 0) {
                // Deletes keys "log:<epoch>:<logId>"
                db_delete_logs(epoch, fromLogId, fromLogId + length - 1);
            }
        }

        // 2. Delete Log Ranges
        // Deletes keys "log_ranges:<tick>" and "tick_log_range:<tick>"
        db_delete_log_ranges(tick);

        // 3. Delete Tick Data
        // Deletes key "tick_data:<tick>"
        db_delete_tick_data(tick);

        // 4. Delete Tick Votes
        // Deletes keys "tick_vote:<tick>:<0..675>"
        db_delete_tick_vote(tick);

        if (tick % 1000 == 0) {
            std::cout << "Processed tick " << tick << std::endl;
        }
    }

    db_close();
    std::cout << "Deletion complete." << std::endl;

    return 0;
}