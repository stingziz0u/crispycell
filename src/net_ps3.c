//
// net_ps3.c -- stub networking module for PS3.
//
// PS3_BUILD has no online multiplayer, so this exists only to satisfy
// the linker for the (dead, unreachable in single-player) -server/
// -connect code paths in d_loop.c. Calling either function is a bug,
// not a real capability gap.
//

#include "net_defs.h"
#include "i_system.h"

static boolean PS3_InitClient(void)
{
    I_Error("Networking is not available in this build.");
    return false;
}

static boolean PS3_InitServer(void)
{
    I_Error("Networking is not available in this build.");
    return false;
}

net_addr_t *PS3_ResolveAddress(const char *addr)
{
    I_Error("Networking is not available in this build.");
    return NULL;
}

static void PS3_AddrToString(net_addr_t *addr, char *buffer, int buffer_len)
{
    buffer[0] = '\0';
}

static void PS3_FreeAddress(net_addr_t *addr)
{
}

static void PS3_SendPacket(net_addr_t *addr, net_packet_t *packet)
{
}

static boolean PS3_RecvPacket(net_addr_t **addr, net_packet_t **packet)
{
    return false;
}

net_module_t net_sdl_module =
{
    PS3_InitClient,
    PS3_InitServer,
    PS3_SendPacket,
    PS3_RecvPacket,
    PS3_AddrToString,
    PS3_FreeAddress,
    PS3_ResolveAddress,
};

void NET_WaitForLaunch(void)
{
    // Unreachable in single player -- see net_ps3.c's header comment
    // and d_loop.c's D_InitNetGame (only called when addr != NULL,
    // which only happens via -server/-connect/-autojoin).
}
