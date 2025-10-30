#include "Config.h"
#include "json/reader.h"
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>

bool LoadConfig(const std::string& path, AppConfig& out, std::string& error) {
    std::ifstream ifs(path);
    if (!ifs) {
        error = "cannot open file";
        return false;
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    const std::string json = buffer.str();

    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

    Json::Value root;
    std::string errs;
    if (!reader->parse(json.data(), json.data() + json.size(), &root, &errs)) {
        error = "invalid JSON: " + errs;
        return false;
    }

    if (!root.isObject()) {
        error = "invalid JSON: root must be an object";
        return false;
    }

    // 'trusted-node' is required and must be an array of strings
    if (!root.isMember("trusted-node") || !root["trusted-node"].isArray()) {
        error = "'trusted-node' array is required";
        return false;
    }
    out.trusted_nodes.clear();
    for (const auto& v : root["trusted-node"]) {
        if (!v.isString()) {
            error = "Invalid type: elements of 'trusted-node' must be strings";
            return false;
        }
        out.trusted_nodes.emplace_back(v.asString());
    }

    // Optional fields (use defaults from AppConfig if absent)
    if (root.isMember("log-level")) {
        if (!root["log-level"].isString()) {
            error = "Invalid type: string required for key 'log-level'";
            return false;
        }
        out.log_level = root["log-level"].asString();
    }

    if (root.isMember("redis-url")) {
        if (!root["redis-url"].isString()) {
            error = "Invalid type: string required for key 'redis-url'";
            return false;
        }
        out.redis_url = root["redis-url"].asString();
    }

    if (root.isMember("arbitrator-identity")) {
        if (!root["arbitrator-identity"].isString()) {
            error = "Invalid type: string required for key 'arbitrator-identity'";
            return false;
        }
        out.arbitrator_identity = root["arbitrator-identity"].asString();
    }

    if (root.isMember("run-server")) {
        if (!root["run-server"].isBool()) {
            error = "Invalid type: boolean required for key 'run-server'";
            return false;
        }
        out.run_server = root["run-server"].asBool();
    }

    if (root.isMember("verify-log-event")) {
        if (!root["verify-log-event"].isBool()) {
            error = "Invalid type: boolean required for key 'verify-log-event'";
            return false;
        }
        out.verify_log_event = root["verify-log-event"].asBool();
    }

    auto validate_uint = [&](const char* key, unsigned& target) -> bool {
        if (!root.isMember(key)) return true;
        const auto& v = root[key];
        if (v.isUInt()) {
            target = v.asUInt();
            return true;
        }
        if (v.isInt()) {
            int i = v.asInt();
            if (i < 0) {
                error = std::string("Negative integer is invalid for key '") + key + "'";
                return false;
            }
            target = static_cast<unsigned>(i);
            return true;
        }
        error = std::string("Invalid type: unsigned integer required for key '") + key + "'";
        return false;
    };

    if (!validate_uint("request-cycle-ms", out.request_cycle_ms)) return false;
    if (!validate_uint("request-logging-cycle-ms", out.request_logging_cycle_ms)) return false;
    if (!validate_uint("future-offset", out.future_offset)) return false;
    if (!validate_uint("server-port", out.server_port)) return false;

    return true;
}
