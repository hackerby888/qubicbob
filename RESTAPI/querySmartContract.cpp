#include <atomic>
#include "connection/connection.h"
#include "shim.h"

void querySmartContractThread(ConnectionPool& connPoolAll, std::atomic_bool& stopFlag)
{
    std::vector<uint8_t> buffer;
    buffer.reserve(0xffffff);
    uint32_t size = 0;
    while (!stopFlag.load())
    {
        buffer.resize(0xffffff);
        MRB_SC.GetPacket(buffer.data(), size);
        buffer.resize(size);
        if (size)
        {
            auto header = (RequestResponseHeader*)buffer.data();
            if (header->size() == size && header->type() == RequestContractFunction::type)
            {
                connPoolAll.sendToRandomBM(buffer.data(), buffer.size());
            }
        }
    }
}