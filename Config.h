#pragma once

#include <string>
#include <vector>

struct AppConfig {
    std::vector<std::string> trusted_nodes;
    unsigned int request_cycle_ms = 1000;
    unsigned int request_logging_cycle_ms = 100;
    unsigned int future_offset = 1;
    std::string log_level = "info";
    std::string keydb_url = "tcp://127.0.0.1:6379";
    bool run_server = false;
    bool is_testnet = false;
    unsigned int server_port = 21842;
    std::string arbitrator_identity;

    bool is_trusted_node = false;
    std::string node_seed = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
};

// Returns true on success; on failure returns false and fills error with a human-readable message.
bool LoadConfig(const std::string& path, AppConfig& out, std::string& error);