
#include <drogon/HttpClient.h>
#include <json/json.h>
#include <vector>
#include <string>

std::vector<std::string> GetPeerFromDNS()
{
    std::vector<std::string> results;

    auto client = drogon::HttpClient::newHttpClient("https://api.qubic.global");
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/random-peers?service=bobNode");

    auto [result, response] = client->sendRequest(req);

    if (result == drogon::ReqResult::Ok && response)
    {
        auto jsonPtr = response->getJsonObject();
        if (jsonPtr && jsonPtr->isMember("bobPeers"))
        {
            const auto& peers = (*jsonPtr)["bobPeers"];
            if (peers.isArray())
            {
                for (const auto& peer : peers)
                {
                    if (peer.isString())
                    {
                        std::string ip = peer.asString();
                        std::string peerString = "bob:" + ip + ":21842:0-0-0-0";
                        results.push_back(peerString);
                    }
                }
            }
        }
    }

    return results;
}