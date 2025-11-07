#pragma once
#include <cstdint>
#include <string>
//#ifdef __cplusplus
//extern "C" {
//#endif
int runBob(int argc, char *argv[]);
void requestToExitBob();

// other APIs:
// - human readable
// - easy for SC dev
void startRESTServer();
void stopRESTServer();
std::string bobGetBalance(const char* identity);
std::string bobGetAsset(const char* identity);
std::string bobGetTransaction(const char* txHash);
std::string bobGetLog(uint16_t epoch, int64_t start, int64_t end); // inclusive
std::string bobGetTick(const uint32_t tick); // return Data And Votes and LogRanges
std::string bobFindLog(uint32_t scIndex, uint32_t logType,
                       const std::string& st1, const std::string& st2, const std::string& st3,
                       uint32_t fromTick, uint32_t toTick);
std::string bobGetStatus();
std::string querySmartContract(uint32_t nonce, uint32_t scIndex, uint32_t funcNumber, uint8_t* data, uint32_t dataSize);
// no one request for C ABI atm, add later if needed
//#ifdef __cplusplus
//}
//#endif