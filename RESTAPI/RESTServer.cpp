#include "drogon/drogon.h"
#include "drogon/HttpAppFramework.h"
#include "drogon/HttpResponse.h"
#include "drogon/utils/Utilities.h"

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

#include "bob.h"
#include "Logger.h"

namespace {
    std::once_flag g_startOnce;
    std::atomic<bool> g_started{false};

    drogon::HttpResponsePtr makeJsonResponse(const std::string& jsonStr, drogon::HttpStatusCode code = drogon::k200OK) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(code);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(jsonStr);
        return resp;
    }

    drogon::HttpResponsePtr makeError(const std::string& msg, drogon::HttpStatusCode code = drogon::k400BadRequest) {
        Json::Value err;
        err["ok"] = false;
        err["error"] = msg;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(code);
        return resp;
    }

    void registerRoutes() {
        using namespace drogon;

        // GET /balance/{identity}
        app().registerHandler(
            "/balance/{1}",
            [](const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, const std::string& identity) {
                try {
                    std::string result = bobGetBalance(identity.c_str());
                    callback(makeJsonResponse(result));
                } catch (const std::exception& ex) {
                    callback(makeError(std::string("balance error: ") + ex.what(), k500InternalServerError));
                }
            },
            {Get}
        );

        // GET /tx/{tx_hash}
        app().registerHandler(
            "/tx/{1}",
            [](const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, const std::string& txHash) {
                try {
                    std::string result = bobGetTransaction(txHash.c_str());
                    callback(makeJsonResponse(result));
                } catch (const std::exception& ex) {
                    callback(makeError(std::string("tx error: ") + ex.what(), k500InternalServerError));
                }
            },
            {Get}
        );

        // GET /log/{epoch}/{from_id}/{to_id}
        app().registerHandler(
                "/log/{1}/{2}/{3}",
                [](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback,
                   const std::string &epochStr, const std::string &fromStr, const std::string &toStr) {
                    try {
                        // Parse as 64-bit integers
                        uint16_t epoch = std::stoll(epochStr);
                        int64_t fromId = std::stoll(fromStr);
                        int64_t toId = std::stoll(toStr);
                        if (toId < fromId) {
                            callback(makeError("to_id must be >= from_id"));
                            return;
                        }
                        std::string result = bobGetLog(epoch, fromId, toId);
                        callback(makeJsonResponse(result));
                    } catch (const std::invalid_argument &) {
                        callback(makeError("from_id/to_id must be integers"));
                    } catch (const std::out_of_range &) {
                        callback(makeError("from_id/to_id out of range"));
                    } catch (const std::exception &ex) {
                        callback(makeError(std::string("log error: ") + ex.what(), k500InternalServerError));
                    }
                },
                {Get}
        );

        // GET /tick/{tick_number}
        app().registerHandler(
            "/tick/{1}",
            [](const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, const std::string& tickStr) {
                try {
                    unsigned long long v = std::stoull(tickStr);
                    if (v > std::numeric_limits<uint32_t>::max()) {
                        callback(makeError("tick number out of uint32 range"));
                        return;
                    }
                    uint32_t tickNum = static_cast<uint32_t>(v);
                    std::string result = bobGetTick(tickNum);
                    callback(makeJsonResponse(result));
                } catch (const std::invalid_argument&) {
                    callback(makeError("tick_number must be an integer"));
                } catch (const std::out_of_range&) {
                    callback(makeError("tick_number out of range"));
                } catch (const std::exception& ex) {
                    callback(makeError(std::string("tick error: ") + ex.what(), k500InternalServerError));
                }
            },
            {Get}
        );
        app().registerHandler(
                "/findLog/{1}/{2}/{3}/{4}/{5}/{6}/{7}",
                [](const HttpRequestPtr& req,
                   std::function<void (const HttpResponsePtr &)> &&callback,
                   const std::string& fromTickStr,
                   const std::string& toTickStr,
                   const std::string& scIndexStr,
                   const std::string& logTypeStr,
                   const std::string& topic1Str,
                   const std::string& topic2Str,
                   const std::string& topic3Str) {
                    try {
                        auto parseU32 = [](const std::string& s, const char* name) -> uint32_t {
                            unsigned long long v = std::stoull(s);
                            if (v > std::numeric_limits<uint32_t>::max()) {
                                throw std::out_of_range(std::string(name) + " out of uint32 range");
                            }
                            return static_cast<uint32_t>(v);
                        };

                        uint32_t fromTick = parseU32(fromTickStr, "fromTick");
                        uint32_t toTick   = parseU32(toTickStr,   "toTick");
                        uint32_t scIndex  = parseU32(scIndexStr,  "SC_INDEX");
                        uint32_t logType  = parseU32(logTypeStr,  "LOG_TYPE");

                        if (fromTick > toTick) {
                            callback(makeError("fromTick must be <= toTick"));
                            return;
                        }

                        std::string result = bobFindLog(scIndex, logType,
                                                        topic1Str, topic2Str, topic3Str,
                                                        fromTick, toTick);
                        callback(makeJsonResponse(result));
                    } catch (const std::invalid_argument&) {
                        callback(makeError("All numeric path params must be integers: fromTick, toTick, SC_INDEX, LOG_TYPE"));
                    } catch (const std::out_of_range& ex) {
                        callback(makeError(ex.what()));
                    } catch (const std::exception& ex) {
                        callback(makeError(std::string("findLog error: ") + ex.what(), k500InternalServerError));
                    }
                },
                {Get}
        );

        // GET /status - Returns node status information
        app().registerHandler(
                "/status",
                [](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
                    try {
                        std::string result = bobGetStatus();
                        callback(makeJsonResponse(result));
                    } catch (const std::exception &ex) {
                        callback(makeError(std::string("status error: ") + ex.what(), k500InternalServerError));
                    }
                },
                {Get}
        );

        // POST /querySmartContract
        app().registerHandler(
                "/querySmartContract",
                [](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
                    try {
                        auto jsonPtr = req->getJsonObject();
                        if (!jsonPtr) {
                            callback(makeError("Invalid or missing JSON body"));
                            return;
                        }
                        const auto &j = *jsonPtr;

                        // Validate required fields and types
                        if (!j.isMember("nonce") || !j["nonce"].isUInt64()) {
                            callback(makeError("nonce (uint64) is required"));
                            return;
                        }
                        if (!j.isMember("scIndex") || !j["scIndex"].isUInt()) {
                            callback(makeError("scIndex (uint32) is required"));
                            return;
                        }
                        if (!j.isMember("funcNumber") || !j["funcNumber"].isUInt()) {
                            callback(makeError("funcNumber (uint32) is required"));
                            return;
                        }
                        if (!j.isMember("data") || !j["data"].isString()) {
                            callback(makeError("data (hex string) is required"));
                            return;
                        }

                        uint64_t nonce = j["nonce"].asUInt64();
                        unsigned long long scIndexULL = j["scIndex"].asUInt();
                        unsigned long long funcNumberULL = j["funcNumber"].asUInt();
                        const std::string dataHex = j["data"].asString();

                        // Range check for uint32
                        if (scIndexULL > std::numeric_limits<uint32_t>::max()) {
                            callback(makeError("scIndex out of uint32 range"));
                            return;
                        }
                        if (funcNumberULL > std::numeric_limits<uint32_t>::max()) {
                            callback(makeError("funcNumber out of uint32 range"));
                            return;
                        }

                        // Basic hex validation for data
                        auto isHex = [](char c) {
                            return (c >= '0' && c <= '9') ||
                                   (c >= 'a' && c <= 'f') ||
                                   (c >= 'A' && c <= 'F');
                        };
                        std::string hex = dataHex;
                        if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
                            hex = hex.substr(2);
                        }
                        if (!hex.empty()) {
                            if (hex.size() % 2 != 0) {
                                callback(makeError("data hex length must be even"));
                                return;
                            }
                            for (char c : hex) {
                                if (!isHex(c)) {
                                    callback(makeError("data must be a hex string"));
                                    return;
                                }
                            }
                        }

                        // At this point input is validated. Implementation hook goes here.
                        // If/when query logic is implemented, replace response below with the actual result.
                        Json::Value resp;
                        resp["ok"] = false;
                        resp["error"] = "querySmartContract not implemented";
                        resp["nonce"] = Json::UInt64(nonce);
                        resp["scIndex"] = Json::UInt(scIndexULL);
                        resp["funcNumber"] = Json::UInt(funcNumberULL);
                        resp["data"] = dataHex;

                        auto httpResp = drogon::HttpResponse::newHttpJsonResponse(resp);
                        httpResp->setStatusCode(drogon::k501NotImplemented);
                        callback(httpResp);
                    } catch (const std::exception &ex) {
                        callback(makeError(std::string("querySmartContract error: ") + ex.what(), k500InternalServerError));
                    }
                },
                {Post}
        );

    }

    void startServerIfNeeded() {
        std::call_once(g_startOnce, []() {
            if (g_started.exchange(true)) return;

            registerRoutes();

            // Configure and start Drogon
            drogon::app()
                .setLogLevel(trantor::Logger::kInfo)
                .addListener("0.0.0.0", 40420)  // listen at port 40420
                .setThreadNum(std::max(2u, std::thread::hardware_concurrency()))
                .disableSigtermHandling();

            // Run Drogon in a background thread so it doesn't block the main program
            std::thread([]() {
                drogon::app().run();
            }).detach();
        });
    }

    // Auto-start the REST server when this translation unit is loaded.
    struct RestAutoStarter {
        RestAutoStarter() { startServerIfNeeded(); }
    } g_autoStarter;
} // namespace

// Optional explicit control APIs (in case another part of the program wants to manage lifecycle)
void startRESTServer() {
    Logger::get()->info("Start REST API server");
    startServerIfNeeded();
}

void stopRESTServer() {
    // This will trigger a graceful shutdown; if the app isn't running, it's a no-op.
    drogon::app().quit();
    Logger::get()->info("Stop REST API server");
}