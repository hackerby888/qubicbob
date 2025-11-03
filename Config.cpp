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

    if (root.isMember("keydb-url")) {
        if (!root["keydb-url"].isString()) {
            error = "Invalid type: string required for key 'keydb-url'";
            return false;
        }
        out.keydb_url = root["keydb-url"].asString();
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

    if (root.isMember("is-testnet")) {
        if (!root["is-testnet"].isBool()) {
            error = "Invalid type: boolean required for key 'is-testnet'";
            return false;
        }
        out.is_testnet = root["is-testnet"].asBool();
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

    if (root.isMember("is-trusted-node")) {
        if (!root["is-trusted-node"].isBool()) {
            error = "Invalid type: boolean required for key 'is-trusted-node'";
            return false;
        }
        out.is_trusted_node = root["is-trusted-node"].asBool();
    }

    if (out.is_trusted_node)
    {
        if (root.isMember("node-seed")) {
            if (!root["node-seed"].isString()) {
                error = "Invalid type: string required for key 'node-seed'";
                return false;
            }
            out.node_seed = root["node-seed"].asString();
        }
    }

    if (root.isMember("trusted-entities")) {
        if (!root["trusted-entities"].isArray()) {
            error = "Invalid type: array required for key 'trusted-entities'";
            return false;
        }
        for (const auto &v: root["trusted-entities"]) {
            if (!v.isString()) {
                error = "Invalid type: elements of 'trusted-entities' must be strings";
                return false;
            }
            const std::string &id = v.asString();
            if (id.length() != 60) {
                error = "Invalid trusted entity ID length: must be 60 characters";
                return false;
            }
            for (char c: id) {
                if (c < 'A' || c > 'Z') {
                    error = "Invalid trusted entity ID format: must be uppercase letters only";
                    return false;
                }
            }
            out.trustedEntities[id] = true;
        }
    }

    return true;
}
