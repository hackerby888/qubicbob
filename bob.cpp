#include "Config.h"
#include "connection/connection.h"
#include "structs.h"
#include "Logger.h"
#include "GlobalVar.h"
#include "database/db.h"
#include "Profiler.h"
#include <chrono>
#include <cstring>   // memcpy
#include <cstdlib>   // strtoull
#include <limits>    // std::numeric_limits
#include <algorithm> // std::max
#include "K12AndKeyUtil.h"
#include <pthread.h> // thread naming on POSIX
#include "shim.h"
#include "bob.h"

void IOVerifyThread(std::atomic_bool& stopFlag);
void IORequestThread(ConnectionPool& conn_pool, std::atomic_bool& stopFlag, std::chrono::milliseconds requestCycle, uint32_t futureOffset);
void EventRequestFromTrustedNode(ConnectionPool& connPoolWithPwd, std::atomic_bool& stopFlag, std::chrono::milliseconds request_logging_cycle_ms);
void EventRequestFromNormalNodes(ConnectionPool& connPoolNoPwd,
                                 std::atomic_bool& stopFlag,
                                 std::chrono::milliseconds request_logging_cycle_ms);
void connReceiver(QCPtr& conn, const bool isTrustedNode, std::atomic_bool& stopFlag);
void DataProcessorThread(std::atomic_bool& exitFlag);
void RequestProcessorThread(std::atomic_bool& exitFlag);
void verifyLoggingEvent(std::atomic_bool& stopFlag);
void indexVerifiedTicks(std::atomic_bool& stopFlag);
bool cleanRawTick(uint32_t fromTick, uint32_t toTick);
void querySmartContractThread(ConnectionPool& connPoolAll, std::atomic_bool& stopFlag);
std::atomic_bool stopFlag{false};

// Public helpers from QubicServer.cpp
bool StartQubicServer(uint16_t port = 21842);
void StopQubicServer();

static inline void set_this_thread_name(const char* name_in) {
    // Linux allows up to 16 bytes including null terminator
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s", name_in ? name_in : "");
    pthread_setname_np(pthread_self(), buf);
}

void requestToExitBob()
{
    stopFlag = true;
}

void garbageCleaner()
{
    Logger::get()->info("Start garbage cleaner");
    long long lastCleanTick = gCurrentFetchingTick - 1;
    uint32_t lastReportedTick = 0;
    while (!stopFlag.load())
    {
        int count = 0;
        SLEEP(100);
        if (stopFlag.load()) break;
        if (gTickStorageMode == TickStorageMode::LastNTick)
        {
            long long cleanToTick = (long long)(gCurrentVerifyLoggingTick.load()) - 5;
            cleanToTick = std::min(cleanToTick, (long long)(gCurrentVerifyLoggingTick) - 1 - gLastNTickStorage);
            if (lastCleanTick < cleanToTick)
            {
                if (cleanRawTick(lastCleanTick + 1, cleanToTick))
                {
                    lastCleanTick = cleanToTick;
                }
            }
        }
        else if (gTickStorageMode == TickStorageMode::Kvrocks)
        {
            long long cleanToTick = (long long)(gCurrentVerifyLoggingTick.load()) - 5;
            if (lastCleanTick < cleanToTick)
            {
                for (long long t = lastCleanTick+1; t <= cleanToTick; t++)
                {
                    compressTickAndMoveToKVRocks(t);
                }
                Logger::get()->trace("Compressed tick {}->{} to kvrocks", lastCleanTick+1, cleanToTick);
                if (cleanRawTick(lastCleanTick + 1, cleanToTick))
                {
                    lastCleanTick = cleanToTick;
                }
                Logger::get()->trace("Cleaned tick {}->{} in keydb", lastCleanTick+1, cleanToTick);
                if (cleanToTick - lastReportedTick > 1000)
                {
                    Logger::get()->trace("Compressed and cleaned up to tick {}", cleanToTick);
                    lastReportedTick = cleanToTick;
                }
            }
        }
    }
    Logger::get()->info("Exited garbage cleaner");
}

int runBob(int argc, char *argv[])
{
    // Ignore SIGPIPE so write/send on a closed socket doesn't terminate the process.
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, nullptr);
    // Load configuration from JSON
    const std::string config_path = (argc > 1) ? std::string(argv[1]) : std::string("bob.json");
    AppConfig cfg;
    std::string cfg_error;
    if (!LoadConfig(config_path, cfg, cfg_error)) {
        printf("Failed to load config '%s': %s\n", config_path.c_str(), cfg_error.c_str());
        return -1;
    }
    // trace - debug - info - warn - error - fatal
    std::string log_level = cfg.log_level;
    Logger::init(log_level);
    gIsTrustedNode = cfg.is_trusted_node;
    if (cfg.is_trusted_node)
    {
        getSubseedFromSeed((uint8_t*)cfg.node_seed.c_str(), nodeSubseed.m256i_u8);
        getPrivateKeyFromSubSeed(nodeSubseed.m256i_u8, nodePrivatekey.m256i_u8);
        getPublicKeyFromPrivateKey(nodePrivatekey.m256i_u8, nodePublickey.m256i_u8);
        char identity[64] = {0};
        getIdentityFromPublicKey(nodePublickey.m256i_u8, identity, false);
        Logger::get()->info("Trusted node identity: {}", identity);
    }
    gTrustedEntities = cfg.trustedEntities;
    gTickStorageMode = cfg.tick_storage_mode;
    gLastNTickStorage = cfg.last_n_tick_storage;

    // Defaults for new knobs are already in AppConfig
    unsigned int request_cycle_ms = cfg.request_cycle_ms;
    unsigned int request_logging_cycle_ms = cfg.request_logging_cycle_ms;
    unsigned int future_offset = cfg.future_offset;
    // Put redis_url in REDIS_CONNECTION_STRING
    std::string KEYDB_CONNECTION_STRING = cfg.keydb_url;


    // Read server flags
    const bool run_server = cfg.run_server;
    unsigned int server_port_u = cfg.server_port;
    if (run_server) {
        if (server_port_u == 0 || server_port_u > 65535) {
            Logger::get()->critical("Invalid server_port {}. Must be in 1..65535", server_port_u);
            return -1;
        }
        const uint16_t server_port = static_cast<uint16_t>(server_port_u);
        if (!StartQubicServer(server_port)) {
            Logger::get()->critical("Failed to start embedded server on port {}", server_port);
            return -1;
        }
        Logger::get()->info("Embedded server enabled on port {}", server_port);
    }

    {
        db_connect(KEYDB_CONNECTION_STRING);
        uint32_t tick;
        uint16_t epoch;
        db_get_latest_tick_and_epoch(tick, epoch);
        gCurrentFetchingTick = tick;
        gCurrentProcessingEpoch = epoch;
        uint16_t event_epoch;
        db_get_latest_event_tick_and_epoch(tick, event_epoch);
        gCurrentFetchingLogTick = tick;
        Logger::get()->info("Loaded DB. DATA: Tick: {} | epoch: {}", gCurrentFetchingTick.load(), gCurrentProcessingEpoch.load());
        Logger::get()->info("Loaded DB. EVENT: Tick: {} | epoch: {}", gCurrentFetchingLogTick.load(), event_epoch);
    }
    if (gTickStorageMode == TickStorageMode::Kvrocks)
    {
        db_kvrocks_connect(cfg.kvrocks_url);
        Logger::get()->info("Connected to kvrocks");
    }
    // Collect endpoints from config
    ConnectionPool connPoolAll;
    ConnectionPool connPoolTrustedNode; // conn pool with passcode
    ConnectionPool connPoolP2P;
    parseConnection(connPoolAll, connPoolTrustedNode, connPoolP2P, cfg.trusted_nodes);
    parseConnection(connPoolAll, connPoolTrustedNode, connPoolP2P, cfg.p2p_nodes);
    if (connPoolAll.size() == 0)
    {
        Logger::get()->error("0 valid connection");
        exit(1);
    }


    uint32_t initTick = 0;
    uint16_t initEpoch = 0;
    uint32_t endEpochTick = 0;
    std::string key = "end_epoch_tick:" + std::to_string(gCurrentProcessingEpoch);
    bool isThisEpochAlreadyEnd = db_get_u32(key, endEpochTick);
    while (initTick == 0 ||
            ( (initEpoch < gCurrentProcessingEpoch && !isThisEpochAlreadyEnd) ||
              (initEpoch <= gCurrentProcessingEpoch && isThisEpochAlreadyEnd)
            )
    )
    {
        doHandshakeAndGetBootstrapInfo(connPoolTrustedNode, true, initTick, initEpoch);
        doHandshakeAndGetBootstrapInfo(connPoolP2P, false, initTick, initEpoch);
        if (isThisEpochAlreadyEnd) Logger::get()->info("Waiting for new epoch info from peers | PeerInitTick: {} PeerInitEpoch {}...", initTick, initEpoch);
        else Logger::get()->info("Doing handshakes and ask for bootstrap info | PeerInitTick: {} PeerInitEpoch {}...", initTick, initEpoch);
        if (initTick == 0 || initEpoch <= gCurrentProcessingEpoch) SLEEP(1000);
    }
    db_insert_u32("init_tick:"+std::to_string(initEpoch), initTick);
    gInitialTick = initTick;
    if (initTick > gCurrentFetchingTick.load())
    {
        gCurrentFetchingTick = initTick;
    }
    if (initTick > gCurrentFetchingLogTick.load())
    {
        gCurrentFetchingLogTick = initTick;
    }

    if (initEpoch > gCurrentProcessingEpoch.load())
    {
        gCurrentProcessingEpoch = initEpoch;
    }

    if (computorsList.epoch != gCurrentProcessingEpoch.load())
    {
        while (computorsList.epoch != gCurrentProcessingEpoch.load())
        {
            getComputorList(connPoolAll, cfg.arbitrator_identity);
            SLEEP(1000);
        }
    }

    stopFlag.store(false);
    auto request_thread = std::thread(
            [&](){
                set_this_thread_name("io-req");
                IORequestThread(
                        std::ref(connPoolAll),
                        std::ref(stopFlag),
                        std::chrono::milliseconds(request_cycle_ms),
                        static_cast<uint32_t>(future_offset)
                );
            }
    );
    auto verify_thread = std::thread([&](){
        set_this_thread_name("verify");
        IOVerifyThread(std::ref(stopFlag));
    });
    auto log_request_trusted_nodes_thread = std::thread([&](){
        set_this_thread_name("trusted-log-req");
        EventRequestFromTrustedNode(std::ref(connPoolTrustedNode), std::ref(stopFlag),
                                    std::chrono::milliseconds(request_logging_cycle_ms));
    });
    auto log_request_p2p_thread = std::thread([&](){
        set_this_thread_name("trusted-log-req");
        EventRequestFromNormalNodes(std::ref(connPoolP2P), std::ref(stopFlag),
                                    std::chrono::milliseconds(request_logging_cycle_ms));
    });
    auto indexer_thread = std::thread([&](){
        set_this_thread_name("indexer");
        indexVerifiedTicks(std::ref(stopFlag));
    });
    auto sc_thread = std::thread([&](){
        set_this_thread_name("sc");
        querySmartContractThread(connPoolAll, std::ref(stopFlag));
    });
    int pool_size = connPoolAll.size();
    std::vector<std::thread> v_recv_thread;
    std::vector<std::thread> v_data_thread;
    Logger::get()->info("Starting {} data processor threads", pool_size);
    const bool isTrustedNode = true;
    for (int i = 0; i < pool_size; i++)
    {
        v_recv_thread.emplace_back([&, i](){
            char nm[16];
            std::snprintf(nm, sizeof(nm), "recv-%d", i);
            set_this_thread_name(nm);
            connReceiver(std::ref(connPoolAll.get(i)), isTrustedNode, std::ref(stopFlag));
        });
    }
    for (int i = 0; i < std::max(4, pool_size); i++)
    {
        v_data_thread.emplace_back([&](){
            set_this_thread_name("data");
            DataProcessorThread(std::ref(stopFlag));
        });
        v_data_thread.emplace_back([&, i](){
            char nm[16];
            std::snprintf(nm, sizeof(nm), "reqp-%d", i);
            set_this_thread_name(nm);
            RequestProcessorThread(std::ref(stopFlag));
        });
    }
    std::thread log_event_verifier_thread;
    log_event_verifier_thread = std::thread([&](){
        set_this_thread_name("log-ver");
        verifyLoggingEvent(std::ref(stopFlag));
    });
    startRESTServer();
    std::thread garbage_thread;
    if (cfg.tick_storage_mode != TickStorageMode::Free)
    {
        garbage_thread = std::thread(garbageCleaner);
    }


    uint32_t prevFetchingTickData = 0;
    uint32_t prevLoggingEventTick = 0;
    uint32_t prevVerifyEventTick = 0;
    uint32_t prevIndexingTick = 0;
    const long long sleep_time = 5;
    auto start_time = std::chrono::high_resolution_clock::now();
    while (!stopFlag.load())
    {
        auto current_time = std::chrono::high_resolution_clock::now();
        float duration_ms = float(std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time).count());
        start_time = std::chrono::high_resolution_clock::now();

        float fetching_td_speed = (prevFetchingTickData == 0) ? 0: float(gCurrentFetchingTick.load() - prevFetchingTickData) / duration_ms * 1000.0f;
        float fetching_le_speed = (prevLoggingEventTick == 0) ? 0: float(gCurrentFetchingLogTick.load() - prevLoggingEventTick) / duration_ms * 1000.0f;
        float verify_le_speed = (prevVerifyEventTick == 0) ? 0: float(gCurrentVerifyLoggingTick.load() - prevVerifyEventTick) / duration_ms * 1000.0f;
        float indexing_speed = (prevIndexingTick == 0) ? 0: float(gCurrentIndexingTick.load() - prevIndexingTick) / duration_ms * 1000.0f;
        prevFetchingTickData = gCurrentFetchingTick.load();
        prevLoggingEventTick = gCurrentFetchingLogTick.load();
        prevVerifyEventTick = gCurrentVerifyLoggingTick.load();
        prevIndexingTick = gCurrentIndexingTick.load();
        Logger::get()->info(
                "Current state: FetchingTick: {} ({:.1f}) | FetchingLog: {} ({:.1f}) | Indexing: {} ({:.1f}) | Verifying: {} ({:.1f})",
                gCurrentFetchingTick.load(), fetching_td_speed,
                gCurrentFetchingLogTick.load(), fetching_le_speed,
                gCurrentIndexingTick.load(), indexing_speed,
                gCurrentVerifyLoggingTick.load(), verify_le_speed);
        requestMapperFrom.clean();
        requestMapperTo.clean();
        responseSCData.clean(10);

        int count = 0;
        while (count++ < sleep_time*10 && !stopFlag.load()) SLEEP(100);
    }
    // Signal stop, disconnect sockets first to break any blocking I/O.
    for (int i = 0; i < connPoolAll.size(); i++) connPoolAll.get(i)->disconnect();
    // Stop and join producer/request threads first so they cannot enqueue more work.
    verify_thread.join();
    Logger::get()->info("Exited Verifying thread");
    request_thread.join();
    Logger::get()->info("Exited TickDataRequest thread");
    log_request_trusted_nodes_thread.join();
    Logger::get()->info("Exited LogEventRequestTrustedNodes thread");
    log_request_p2p_thread.join();
    Logger::get()->info("Exited LogEventRequestP2P thread");
    indexer_thread.join();
    Logger::get()->info("Exited indexer thread");
    sc_thread.join();
    if (log_event_verifier_thread.joinable())
    {
        Logger::get()->info("Exiting verifyLoggingEvent thread");
        log_event_verifier_thread.join();
        Logger::get()->info("Exited verifyLoggingEvent thread");
    }

    // Now the receivers can drain and exit.
    for (auto& thr : v_recv_thread) thr.join();
    Logger::get()->info("Exited recv threads");

    // Wake all data threads so none remain blocked on MRB.
    {
        const size_t wake_count = v_data_thread.size() * 4; // ensure enough tokens
        std::vector<RequestResponseHeader> tokens(wake_count);
        for (auto& t : tokens) {
            t.randomizeDejavu();
            t.setType(35); // NOP
            t.setSize(8);
        }
        for (size_t i = 0; i < wake_count; ++i) {
            MRB_Data.EnqueuePacket(reinterpret_cast<uint8_t*>(&tokens[i]));
            MRB_Request.EnqueuePacket(reinterpret_cast<uint8_t*>(&tokens[i]));
        }

        // Keep tokens alive until all data threads exit
        for (auto& thr : v_data_thread) thr.join();
    }
    Logger::get()->info("Exited data threads");
    if (cfg.tick_storage_mode != TickStorageMode::Free)
    {
        garbage_thread.join();
    }
    if (gIsEndEpoch)
    {
        Logger::get()->info("Received END_EPOCH message. Closing BOB");
    }
    db_close();
    if (gTickStorageMode == TickStorageMode::Kvrocks)
    {
        db_kvrocks_close();
    }
    // Stop embedded server (if it was started) before shutting down logger
    StopQubicServer();
    stopRESTServer();
    ProfilerRegistry::instance().printSummary();
    Logger::get()->info("Shutting down logger");
    spdlog::shutdown();
    return 0;
}