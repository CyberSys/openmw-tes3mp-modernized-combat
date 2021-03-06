#ifndef OPENMW_PROCESSOROBJECTACTIVATE_HPP
#define OPENMW_PROCESSOROBJECTACTIVATE_HPP

#include "../ObjectProcessor.hpp"
#include <apps/openmw-mp/Networking.hpp>

namespace mwmp
{
    class ProcessorObjectActivate : public ObjectProcessor
    {
    public:
        ProcessorObjectActivate()
        {
            BPP_INIT(ID_OBJECT_ACTIVATE)
        }

        void Do(ObjectPacket &packet, Player &player, BaseObjectList &objectList) override
        {
            LOG_MESSAGE_SIMPLE(MWMPLog::LOG_INFO, "Received %s from %s", strPacketID.c_str(), player.npc.mName.c_str());

            Script::Call<Script::CallbackIdentity("OnObjectActivate")>(player.getId(), objectList.cell.getDescription().c_str());
        }
    };
}

#endif //OPENMW_PROCESSOROBJECTACTIVATE_HPP
