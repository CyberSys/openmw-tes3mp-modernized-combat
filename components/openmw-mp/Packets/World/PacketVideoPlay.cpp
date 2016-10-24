#include <components/openmw-mp/NetworkMessages.hpp>
#include "PacketVideoPlay.hpp"

using namespace mwmp;

PacketVideoPlay::PacketVideoPlay(RakNet::RakPeerInterface *peer) : WorldPacket(peer)
{
    packetID = ID_WORLD_VIDEO_PLAY;
}

void PacketVideoPlay::Packet(RakNet::BitStream *bs, WorldEvent *event, bool send)
{
    WorldPacket::Packet(bs, event, send);

    RW(event->video, send);
    RW(event->allowSkipping, send);
}