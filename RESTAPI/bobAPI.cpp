// interop for other program to interact with BOB
#include "bob.h"
#include "K12AndKeyUtil.h"
#include "shim.h"
#include "Entity.h"
#include "database/db.h"
#include <json/json.h>
#include <vector>
#include <sstream>
#include <iomanip>

// helper: hex-encode
static std::string toHex(const std::vector<uint8_t>& data) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : data) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// Non-blocking enqueue: send a SC query and return immediately
bool enqueueSmartContractRequest(uint32_t nonce, uint32_t scIndex, uint32_t funcNumber, const uint8_t* data, uint32_t dataSize)
{
    std::vector<uint8_t> vdata(dataSize + sizeof(RequestContractFunction) + sizeof(RequestResponseHeader));
    RequestContractFunction rcf{};
    rcf.contractIndex = scIndex;
    rcf.inputSize = dataSize;
    rcf.inputType = funcNumber;

    auto header = (RequestResponseHeader*)vdata.data();
    header->setType(RequestContractFunction::type);
    header->setSize(dataSize + sizeof(RequestResponseHeader) + sizeof(RequestContractFunction));
    header->setDejavu(nonce);

    memcpy(vdata.data() + sizeof(RequestResponseHeader), &rcf, sizeof(RequestResponseHeader));
    if (dataSize)
        memcpy(vdata.data() + sizeof(RequestResponseHeader) + sizeof(RequestContractFunction), data, dataSize);

    // fire-and-forget to SC thread
    return MRB_SC.EnqueuePacket(vdata.data());
}

std::string bobGetBalance(const char* identity)
{
    if (!identity) return "{\"error\": \"Wrong identity format\"}";
    std::string str(identity);
    if (str.size() < 60) return "{\"error\": \"Wrong identity format\"}";

    m256i pk{};
    getPublicKeyFromIdentity(str.data(), pk.m256i_u8);
    int index = spectrumIndex(pk);
    if (index < 0) return "{\"error\": \"Wrong identity format\"}";

    const auto& e = spectrum[index];
    std::string error = "null";
    if (e.numberOfIncomingTransfers > gCurrentVerifyLoggingTick - 1 || e.numberOfOutgoingTransfers > gCurrentVerifyLoggingTick - 1)
    {
        error = "This entity is being processed. currentBobTick is smaller than latestIncomingTransferTick/latestOutgoingTransferTick";
    }
    return std::string("{") +
           "\"incomingAmount\":" + std::to_string(e.incomingAmount) +
            ",\"outgoingAmount\":" + std::to_string(e.outgoingAmount) +
            ",\"balance\":" + std::to_string(e.incomingAmount - e.outgoingAmount) +
           ",\"numberOfIncomingTransfers\":" + std::to_string(e.numberOfIncomingTransfers) +
           ",\"numberOfOutgoingTransfers\":" + std::to_string(e.numberOfOutgoingTransfers) +
           ",\"latestIncomingTransferTick\":" + std::to_string(e.latestIncomingTransferTick) +
           ",\"latestOutgoingTransferTick\":" + std::to_string(e.latestOutgoingTransferTick) +
            ",\"currentBobTick:\":" + std::to_string(gCurrentVerifyLoggingTick - 1) +
            ",\"error:\":" + error +
           "}";
}

std::string bobGetAsset(const char* identity)
{
    return "{\"error\": \"Not yet implemented\"}";
}

std::string bobGetTransaction(const char* txHash)
{
    if (!txHash) return "{\"error\": \"Invalid transaction hash\"}";

    try {
        std::vector<uint8_t> txData;
        if (!db_get_transaction(txHash, txData)) {
            return "{\"error\": \"Transaction not found\"}";
        }
        Transaction *tx = reinterpret_cast<Transaction *>(txData.data());
        if (!tx) {
            return "{\"error\": \"Invalid transaction data\"}";
        }
        std::string inputData = "";
        if (tx->inputSize)
        {
            const uint8_t *input = txData.data() + sizeof(Transaction);
            std::stringstream ss;
            ss << std::hex << std::setfill('0');
            for (size_t i = 0; i < tx->inputSize; ++i) {
                ss << std::setw(2) << static_cast<int>(input[i]);
            }
            inputData = ss.str();
        }
        int tx_index;
        long long from_log_id;
        long long to_log_id;
        uint64_t timestamp;
        bool executed;
        if (!db_get_indexed_tx(txHash, tx_index, from_log_id, to_log_id, timestamp, executed)) {
            return std::string("{") +
                   "\"hash\":\"" + txHash + "\"," +
                   "\"from\":\"" + getIdentity(tx->sourcePublicKey, false) + "\"," +
                   "\"to\":\"" + getIdentity(tx->destinationPublicKey, false) + "\"," +
                   "\"amount\":" + std::to_string(tx->amount) + "," +
                   "\"tick\":" + std::to_string(tx->tick) + "," +
                    "\"inputSize\":" + std::to_string(tx->inputSize) + "," +
                    "\"inputType\":" + std::to_string(tx->inputType) + "," +
                    "\"inputData\":\"" + inputData + "\"" +
                    "}";
        }

        return std::string("{") +
               "\"hash\":\"" + txHash + "\"," +
               "\"from\":\"" + getIdentity(tx->sourcePublicKey, false) + "\"," +
               "\"to\":\"" + getIdentity(tx->destinationPublicKey, false) + "\"," +
               "\"amount\":" + std::to_string(tx->amount) + "," +
               "\"tick\":" + std::to_string(tx->tick) + "," +
                "\"logIdFrom\":" + std::to_string(from_log_id) + "," +
                "\"logIdTo\":" + std::to_string(to_log_id) + "," +
                "\"transactionIndex\":" + std::to_string(tx_index) + "," +
                "\"executed\":" + (executed ? "true" : "false") + "," +
                "\"timestamp\":" + std::to_string(timestamp) + "," +
                "\"inputSize\":" + std::to_string(tx->inputSize) + "," +
                "\"inputType\":" + std::to_string(tx->inputType) + "," +
                "\"inputData\":\"" + inputData + "\"" +
                "}";
    } catch (const std::exception &e) {
        return std::string("{\"error\": \"") + e.what() + "\"}";
    }
}

std::string bobGetLog(uint16_t epoch, int64_t start, int64_t end)
{

    if (start < 0 || end < 0 || end < start) {
        return "{\"error\":\"Wrong range\"}";
    }

    std::string result;
    result.push_back('[');
    bool first = true;

    for (int64_t id = start; id <= end; ++id) {
        LogEvent log;
        if (db_get_log(epoch, static_cast<uint64_t>(id), log)) {
            std::string js = log.parseToJson();
            if (!first) result.push_back(',');
            result += js;
            first = false;
        } else {
            Json::Value err(Json::objectValue);
            err["ok"] = false;
            err["error"] = "not_found";
            err["epoch"] = epoch;
            err["logId"] = Json::UInt64(static_cast<uint64_t>(id));
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            std::string js = Json::writeString(wb, err);
            if (!first) result.push_back(',');
            result += js;
            first = false;
        }
    }

    result.push_back(']');
    return result;

}


std::string bobGetTick(const uint32_t tick) {
    FullTickStruct fts;
    db_get_vtick(tick, fts);

    Json::Value root;
    root["tick"] = tick;

    // Set TickData -> root["tickdata"]
    const TickData& td = fts.td;
    Json::Value tdJson;
    tdJson["computorIndex"] = td.computorIndex;
    tdJson["epoch"] = td.epoch;
    tdJson["tick"] = td.tick;

    tdJson["millisecond"] = td.millisecond;
    tdJson["second"] = td.second;
    tdJson["minute"] = td.minute;
    tdJson["hour"] = td.hour;
    tdJson["day"] = td.day;
    tdJson["month"] = td.month;
    tdJson["year"] = td.year;

    // m256i fields as hex
    tdJson["timelock"] = td.timelock.toQubicHash();

    // transactionDigests[1024] as hex array
    {
        Json::Value digests(Json::arrayValue);
        for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; ++i) {
            if (td.transactionDigests[i] != m256i::zero())
                digests.append(td.transactionDigests[i].toQubicHash());
        }
        tdJson["transactionDigests"] = digests;
    }

    // contractFees[1024] as numeric array
    {
        bool nonZero = false;
        Json::Value fees(Json::arrayValue);
        for (int i = 0; i < 1024; ++i) {
            fees.append(static_cast<Json::Int64>(td.contractFees[i]));
            if (td.contractFees[i]) nonZero = true;
        }
        if (nonZero) tdJson["contractFees"] = fees;
        else tdJson["contractFees"] = 0;
    }

    // signature as hex
    tdJson["signature"] = byteToHexStr(td.signature, 64);

    root["tickdata"] = tdJson;

    // Add TickVote array (minimal fields, keep signatures as hex)
    Json::Value votes(Json::arrayValue);
    for (const auto &vote : fts.tv) {
        Json::Value voteObj;
        // Basic info
        voteObj["computorIndex"] = vote.computorIndex;
        voteObj["epoch"] = vote.epoch;
        voteObj["tick"] = vote.tick;

        // Timestamp fields
        voteObj["millisecond"] = vote.millisecond;
        voteObj["second"] = vote.second;
        voteObj["minute"] = vote.minute;
        voteObj["hour"] = vote.hour;
        voteObj["day"] = vote.day;
        voteObj["month"] = vote.month;
        voteObj["year"] = vote.year;

        // Digest integers
        voteObj["prevResourceTestingDigest"] = vote.prevResourceTestingDigest;
        voteObj["saltedResourceTestingDigest"] = vote.saltedResourceTestingDigest;
        voteObj["prevTransactionBodyDigest"] = vote.prevTransactionBodyDigest;
        voteObj["saltedTransactionBodyDigest"] = vote.saltedTransactionBodyDigest;

        // m256i digests (use toQubicHash())
        voteObj["prevSpectrumDigest"] = vote.prevSpectrumDigest.toQubicHash();
        voteObj["prevUniverseDigest"] = vote.prevUniverseDigest.toQubicHash();
        voteObj["prevComputerDigest"] = vote.prevComputerDigest.toQubicHash();
        voteObj["saltedSpectrumDigest"] = vote.saltedSpectrumDigest.toQubicHash();
        voteObj["saltedUniverseDigest"] = vote.saltedUniverseDigest.toQubicHash();
        voteObj["saltedComputerDigest"] = vote.saltedComputerDigest.toQubicHash();

        voteObj["transactionDigest"] = vote.transactionDigest.toQubicHash();
        voteObj["expectedNextTickTransactionDigest"] = vote.expectedNextTickTransactionDigest.toQubicHash();

        // Signature as hex
        voteObj["signature"] = byteToHexStr(vote.signature, SIGNATURE_SIZE);

        votes.append(voteObj);
    }
    root["votes"] = votes;

    // Convert to string
    Json::FastWriter writer;
    return writer.write(root);
}


std::string bobFindLog(uint32_t scIndex, uint32_t logType,
                       const std::string& t1, const std::string& t2, const std::string& t3,
                       uint32_t fromTick, uint32_t toTick)
{
    if (fromTick > toTick) {
        return "{\"error\":\"Wrong range\"}";
    }

    if (!t1.empty() && t1.length() != 60) {
        return "{\"error\":\"Invalid length topic1\"}";
    }
    if (!t2.empty() && t2.length() != 60) {
        return "{\"error\":\"Invalid length topic2\"}";
    }
    if (!t3.empty() && t3.length() != 60) {
        return "{\"error\":\"Invalid length topic3\"}";
    }
    std::string st1 = t1,st2 = t2,st3 = t3;
    std::transform(t1.begin(), t1.end(), st1.begin(), ::tolower);
    std::transform(t2.begin(), t2.end(), st2.begin(), ::tolower);
    std::transform(t3.begin(), t3.end(), st3.begin(), ::tolower);

    std::vector<uint32_t> ids = db_search_log(scIndex, logType, fromTick, toTick, st1, st2, st3);

    // Return as a compact JSON array
    std::string result;
    result.push_back('[');
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) result.push_back(',');
        result += std::to_string(ids[i]);
    }
    result.push_back(']');
    return result;
}

std::string bobGetStatus()
{
    return std::string("{") +
           "\"currentProcessingEpoch\":" + std::to_string(gCurrentProcessingEpoch) +
           ",\"currentFetchingTick\":" + std::to_string(gCurrentFetchingTick) +
           ",\"currentFetchingLogTick\":" + std::to_string(gCurrentFetchingLogTick) +
           ",\"currentVerifyLoggingTick\":" + std::to_string(gCurrentVerifyLoggingTick) +
           ",\"currentIndexingTick\":" + std::to_string(gCurrentIndexingTick) +
            ",\"initialTick\":" + std::to_string(gInitialTick) +
           "}";
}

std::string querySmartContract(uint32_t nonce, uint32_t scIndex, uint32_t funcNumber, uint8_t* data, uint32_t dataSize)
{
    // Preserve existing sync API for backwards compatibility,
    // but avoid blocking the thread for long: do a single immediate check, else enqueue and return pending.
    std::vector<uint8_t> dataOut;
    Json::Value root;

    if (responseSCData.get(nonce, dataOut)) {
        root["nonce"] = nonce;
        root["data"] = toHex(dataOut);
    } else {
        enqueueSmartContractRequest(nonce, scIndex, funcNumber, data, dataSize);
        root["error"] = "pending";
        root["message"] = "Query enqueued; try again shortly with the same nonce";
    }

    Json::FastWriter writer;
    return writer.write(root);
}


std::string broadcastTransaction(uint8_t* txDataWithHeader, int size)
{
    auto tx = (Transaction*)(txDataWithHeader+sizeof(RequestResponseHeader));
    if (tx->inputSize + sizeof(Transaction) + sizeof(RequestResponseHeader) + SIGNATURE_SIZE != size)
    {
        return "{\"error\": \"Invalid size\"}";
    }
    m256i digest{};
    uint8_t* signature = txDataWithHeader + sizeof(RequestResponseHeader) + sizeof(Transaction) + tx->inputSize;
    KangarooTwelve(reinterpret_cast<const uint8_t *>(tx), size - sizeof(RequestResponseHeader) - SIGNATURE_SIZE, digest.m256i_u8, 32);
    if (!verify(tx->sourcePublicKey, digest.m256i_u8, signature))
    {
        return "{\"error\": \"Invalid signature\"}";
    }
    MRB_SC.EnqueuePacket(txDataWithHeader);
    KangarooTwelve(reinterpret_cast<const uint8_t *>(tx), size - sizeof(RequestResponseHeader), digest.m256i_u8, 32);
    char hash[64]={0};
    getIdentityFromPublicKey(digest.m256i_u8, hash, true);
    std::string txHash(hash);
    return "{\"txHash\": \"" + txHash + "\"}";
}

std::string bobGetEpochInfo(uint16_t epoch)
{
    Json::Value root;
    uint32_t end_epoch_tick = 0;
    auto es = std::to_string(epoch);
    long long length = -1, start = -1;
    uint32_t initTick = 0;
    db_get_u32("end_epoch_tick:" + es, end_epoch_tick);
    db_get_end_epoch_log_range(epoch, start, length);
    db_get_u32("init_tick:"+std::to_string(epoch), initTick);

    root["epoch"] = epoch;
    root["initialTick"] = initTick;
    root["endTick"] = end_epoch_tick;
    root["endTickStartLogId"] = Json::Int64(start);
    root["endTickEndLogId"] = Json::Int64(start + length - 1);
    Json::FastWriter writer;
    return writer.write(root);
}