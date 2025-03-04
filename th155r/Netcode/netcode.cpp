#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <winsock2.h>
#include <windows.h>
#include <squirrel.h>

#include <vector>

#include "util.h"
#include "patch_utils.h"
#include "fake_lag.h"
#include "netcode.h"
#include "log.h"
#include "lobby.h"
#include "config.h"

char punch_ip_buffer[INET6_ADDRSTRLEN] = "";
size_t punch_ip_len = 0;
bool punch_ip_updated = false;

static inline constexpr bool is_ipv6_compatible_with_ipv4(const IP6_ADDRESS& addr) {
    return addr.IP6Dword[0] == 0 && addr.IP6Dword[1] == 0 && addr.IP6Dword[2] == 0xFFFF0000;
}

template <typename T>
static int sprint_ipv4(T* buf, IP4_ADDRESS addr) {
    T* buf_write = buf;

    /*
    buf_write += uint8_to_strbuf(addr, buf_write);
    *buf_write++ = (T)'.';
    addr >>= 8;
    buf_write += uint8_to_strbuf(addr, buf_write);
    *buf_write++ = (T)'.';
    addr >>= 8;
    buf_write += uint8_to_strbuf(addr, buf_write);
    *buf_write++ = (T)'.';
    addr >>= 8;
    buf_write += uint8_to_strbuf(addr, buf_write);
    */

    size_t i = 4;
    while (true) {
        buf_write += uint8_to_strbuf(addr, buf_write);
        if (--i == 0) break;
        *buf_write++ = (T)'.';
        addr >>= 8;
    }
    return buf_write - buf;
}

template <typename T>
static int sprint_ipv6(T* buf, const IP6_ADDRESS& addr) {
    T* buf_write = buf;

    /*
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[0]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[1]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[2]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[3]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[4]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[5]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[6]), buf_write);
    *buf_write++ = (T)':';
    buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[7]), buf_write);
    */

    size_t i = 0;
    nounroll while (true) {
        buf_write += uint16_to_hex_strbuf(bswap(addr.IP6Word[i]), buf_write);
        if (++i == 8) break;
        *buf_write++ = (T)':';
    }
    return buf_write - buf;
}

template <typename T>
static int sprint_ip(T* buf, bool is_ipv6, const void* addr) {
    IP4_ADDRESS ip4;
    if (is_ipv6) {
        const IP6_ADDRESS& ip6 = *(const IP6_ADDRESS*)addr;
        if (!is_ipv6_compatible_with_ipv4(ip6)) {
            return sprint_ipv6(buf, ip6);
        }
        ip4 = ip6.IP6Dword[3];
    } else {
        ip4 = *(IP4_ADDRESS*)addr;
    }
    return sprint_ipv4(buf, ip4);
}

template <typename T>
static int sprint_ip_and_port(T* buf, bool is_ipv6, const void* addr, uint16_t port) {
    int addr_len = sprint_ip(buf, is_ipv6, addr);
    buf[addr_len++] = (T)':';
    addr_len += uint16_to_strbuf(port, buf + addr_len);
    buf[addr_len] = (T)'\0';
    return addr_len;
}

// size: 0x1C
struct BoostSockAddr {
    SOCKADDR_INET addr = {}; // 0x0
    // 0x1C

    inline size_t length() const {
        switch (this->addr.si_family) {
            case AF_INET:
                return sizeof(sockaddr_in);
            case AF_INET6:
                return sizeof(sockaddr_in6);
            default:
                return 0;
        }
    }

    inline sockaddr_in& addr_v4() {
        return this->addr.Ipv4;
    }

    inline sockaddr_in6& addr_v6() {
        return this->addr.Ipv6;
    }

    inline sockaddr& addr_any() {
        return *(sockaddr*)&this->addr;
    }
};

// size: 0x8
struct BoostMutex {
    std::atomic<uint32_t> active_count; // 0x0
    void* event_handle; // 0x4
    // 0x8
};

// size: 0x4
struct UDPInnerB {
    int __dword_0; // 0x0
    // 0x4
};

// size: 0x14+
struct UDPInnerD {
    void* __ptr_0; // 0x0
    void* __ptr_4; // 0x4
    void* __ptr_8; // 0x8
    void* __ptr_C; // 0xC
    int __dword_10; // 0x10
    // 0x14
};

// size: 0x4
struct UDPInnerE {
    unsigned char unknown_fields[0x4]; // 0x0
    // 0x4
};

// size: 0x98
struct ConnectionData {
    BoostSockAddr addr; // 0x0
    short __word_1C; // 0x1C
    unsigned char probably_padding_bytesA[0x2]; // 0x1E
    int state; // 0x20
    int __int_24; // 0x24
    int __dword_28; // 0x28
    uint32_t __uint_2C; // 0x2C
    int __int_30; // 0x30
    int __int_34; // 0x34
    int __dword_38; // 0x38
    int delay; // 0x3C
    unsigned char __byte_40; // 0x40
    unsigned char probably_padding_bytesB[0x3]; // 0x41
    uint32_t __uint_44; // 0x44 Written as u32, read as u16. QPC related
    std::vector<uint8_t> __vector_48; // 0x48
    std::vector<uint8_t> __vector_54; // 0x54
    int __dword_60; // 0x60 Some sort of bitset thing?
    int __dword_64; // 0x64
    int __dword_68; // 0x68
    int __dword_6C; // 0x6C Another instance of whatever 0x60 is
    int __dword_70; // 0x70
    int __dword_74; // 0x74
    UDPInnerD __innerD_78; // 0x78
    UDPInnerE __innerE_8C; // 0x8C
    BoostMutex __mutex_90; // 0x90
    // 0x98
};

static_assert(sizeof(ConnectionData) == 0x98);

struct TF4UDP {
    void* vftable; // 0x0
    void* __mutex_related_4; // 0x4
    int __dword_8; // 0x8
    void* __ptr_C; // 0xC
    int __dword_array_10[4]; // 0x10
    SOCKET& socket; // 0x20
    int __dword_24; // 0x24
    BoostSockAddr __addr_28; // 0x28
    BoostSockAddr __addr_44; // 0x44
    BoostSockAddr __addr_60; // 0x60
    BoostSockAddr __addr_7C; // 0x7C
    UDPInnerB __innerB_98; // 0x98
    ConnectionData __connection_9C; // 0x9C
    ConnectionData parent; // 0x134
    ConnectionData* child_array; // 0x1CC
    size_t child_array_size; // 0x1D0
    unsigned char __byte_1D4; // 0x1D4
    unsigned char probably_padding_bytesA[0x3]; // 0x1D5
    UDPInnerB __innerB_1D8; // 0x1D8
    std::vector<BoostSockAddr> __addr_vector_1DC; // 0x1DC
    uint32_t __uint_1E8; // 0x1E8
    uint32_t __uint_1EC; // 0x1EC These are counters of some sort
    uint32_t __uint_1F0; // 0x1F0
    BoostMutex __mutex_1F4; // 0x1F4
    std::vector<uint8_t> __vector_1FC; // 0x1FC
    BoostMutex __mutex_208; // 0x208
    unsigned char __byte_210; // 0x210
    unsigned char probably_padding_bytesB[0x3]; // 0x211
    UDPInnerB __innerB_214; // 0x214
    BoostSockAddr recv_addr; // 0x218
    std::vector<uint8_t> recv_data; // 0x234
    void* __ptr_240; // 0x240
    int __dword_244; // 0x244
    void* __manbow_network_impl; // 0x248
    // 0x24C
};

static_assert(sizeof(TF4UDP) == 0x24C);

#define resync_patch_addr (0x0E364C_R)
#define patchA_addr (0x0E357A_R)
#define wsasendto_import_addr (0x3884D4_R)
#define wsarecvfrom_import_addr (0x3884D8_R)
#define packet_parser_addr (0x176BB0_R)

// TODO: Is this variable name inverted?
static bool not_in_match = false;


static uint8_t lag_packets = 0;
SQBool resyncing = SQFalse;
SQBool isplaying = SQFalse;
static uint64_t prev_timestamp = 0;

static inline constexpr uint8_t RESYNC_THRESHOLD = UINT8_MAX;
static inline constexpr uint8_t RESYNC_DURATION = UINT8_MAX;

static inline constexpr PacketPunchPing PUNCH_PING_PACKET = {
    .type = PACKET_TYPE_PUNCH_PING
};

/*
TO FIX:
1.crash after attempting to host/connect
after connection loss due to really bad connection
*/

//resync_logic
//start
static void resync_patch(uint8_t value) {
    DWORD old_protect;
    uint8_t* patch_addr = (uint8_t*)resync_patch_addr;
    if (VirtualProtect(patch_addr, 1, PAGE_READWRITE, &old_protect)) {
        
        static constexpr int8_t value_table[] = {
            5, 10, 15, 30, 45, 90, INT8_MAX, INT8_MAX
        };
        int8_t new_value = value_table[value / 32];
        *patch_addr = new_value;//value_table[value / 16];

        VirtualProtect(patch_addr, 1, old_protect, &old_protect);
    }
}

#define USE_ORIGINAL_RESYNC 1

static void run_resync_logic(uint64_t new_timestamp) {
#if USE_ORIGINAL_RESYNC
    if (!resyncing) {
        if (prev_timestamp != new_timestamp) {
            prev_timestamp = new_timestamp;
            lag_packets = 0;
        }
        else {
            if (++lag_packets >= RESYNC_THRESHOLD) {
                resyncing = SQTrue;
                lag_packets = 0;
            }
        }
    } else {
        if (lag_packets >= RESYNC_DURATION) {
            resyncing = SQFalse;
            lag_packets = 0;
        }
        else {
            resync_patch(lag_packets);
            ++lag_packets;
        }
    }
#else
    uint8_t prev_lag_packets = lag_packets;
    if (prev_timestamp != new_timestamp) {
        prev_timestamp = new_timestamp;

        lag_packets = saturate_sub<uint8_t>(prev_lag_packets, 32u);
    }
    else {
        lag_packets = saturate_add<uint8_t>(prev_lag_packets, 1u);
    }
    if (lag_packets != prev_lag_packets) {
        resync_patch(lag_packets);
    }
#endif
}


int WSAAPI my_WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {

    PacketLayout* packet = (PacketLayout*)lpBuffers[0].buf;

    switch (packet->type) {
        default:
            break;
#if NETPLAY_PATCH_TYPE == NETPLAY_VER_103F
        case PACKET_TYPE_9:
            if (not_in_match) {
                if (lpNumberOfBytesSent) {
                    *lpNumberOfBytesSent = 1;
                }
                return 0;
            }
            not_in_match = true;
            break;
        case PACKET_TYPE_11:
            not_in_match = false;
            break;
        case PACKET_TYPE_18:
            if (lpBuffers[0].len >= 25) {
                run_resync_logic(*(uint64_t*)&packet->data[16]);
            }
            break;
        case PACKET_TYPE_19:
            if (lpBuffers[0].len >= 26) {
                run_resync_logic(*(uint64_t*)&packet->data[17]);
            }
            break;
#endif
    }

    return WSASendTo_log(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
}

typedef void thisfastcall packet_parser_t(
    TF4UDP* self,
    thisfastcall_edx(int dummy_edx,)
    size_t packet_size
);

void thisfastcall packet_parser_hook(
    TF4UDP* self,
    thisfastcall_edx(int dummy_edx,)
    size_t packet_size
) {
    
    PacketLayout* packet_raw = (PacketLayout*)self->recv_data.data();

    recvfrom_log(packet_raw, packet_size, &self->recv_addr.addr_any(), self->recv_addr.length());

    switch (packet_raw->type) {
        default:
            break;
#if NETPLAY_PATCH_TYPE == NETPLAY_VER_103F
        // TODO: These packet numbers don't seem quite
        // right based on the variable name...
        case PACKET_TYPE_0: case PACKET_TYPE_12: case PACKET_TYPE_13:
        case PACKET_TYPE_14: case PACKET_TYPE_15: case PACKET_TYPE_16:
        case PACKET_TYPE_17: case PACKET_TYPE_18: case PACKET_TYPE_19:
            not_in_match = false;
            break;
#endif
        case PACKET_TYPE_PUNCH_PING:
            //sendto(self->socket, (const char*)&PUNCH_PING_PACKET, sizeof(PUNCH_PING_PACKET), 0, &self->recv_addr.addr_any(), self->recv_addr.length());
            break;
        case PACKET_TYPE_PUNCH_SELF: {
            if (addr_is_lobby(&self->recv_addr.addr_any(), self->recv_addr.length())) {
                PacketPunchPeer* packet = (PacketPunchPeer*)packet_raw;
                punch_ip_len = sprint_ip_and_port(punch_ip_buffer, packet->is_ipv6, packet->ip, packet->remote_port);
                punch_ip_updated = true;
            }
            break;
        }
    }
    
    return ((packet_parser_t*)packet_parser_addr)(
        self,
        thisfastcall_edx(dummy_edx,)
        packet_size
    );
}

#if BETTER_BLACK_SCREEN_FIX

struct BoostLock {
    void* mutex_addr;
    bool is_locked;
};

typedef void fastcall lock_func_t(BoostLock* lock);

void fastcall fix_black_screen_lock(BoostLock* lock) {

    uint8_t* mutex_addr = (uint8_t*)lock->mutex_addr;

    auto current_thread = read_fs_dword(0x24);
    if (current_thread != *based_pointer<std::atomic<uint32_t>>(mutex_addr, -0x50)) {
        ((lock_func_t*)(0xF410_R))(lock);
        *based_pointer<std::atomic<uint32_t>>(mutex_addr, -0x50) = current_thread;
    }
    ++mutex_addr[-0x71];
    lock->is_locked = true;
}

void fastcall fix_black_screen_unlock(BoostLock* lock) {

    uint8_t* mutex_addr = (uint8_t*)lock->mutex_addr;
    
    if (expect(!--mutex_addr[-0x71], true)) {
        *based_pointer<std::atomic<uint32_t>>(mutex_addr, -0x50) = 0;
        return ((lock_func_t*)(0xF4C0_R))(lock);
    }
}

static constexpr uintptr_t lock_fix_addrs[] = {
    0x172FD6, // Method 28
    0x178B96, // Method 20
    0x178ED7, // Method 24
    0x1791BF, // Method 10
    0x17CDC9, // Handle packet 9
    0x17CFEB, // Handle packet 11
    0x17D1DB, // Handle packet 12
    0x17D36B, // Handle packet 13
    0x17D5B9, // Handle packet 15
    0x17D687  // Handle packet 16
};
static constexpr uintptr_t unlock_fixA_addrs[] = {
    0x373F74, // Method 28 SEH
    0x3746FC, // Method 20 SEH
    0x37473C, // Method 24 SEH
    0x374774, // Method 10 SEH
    0x17CE02, 0x17CE98, 0x17CEB5, // Handle packet 9
    0x374E04, // Handle packet 9 SEH
    0x374E34  // Handle packet 11 SEH, Handle packet 12 SEH
};

static constexpr uintptr_t unlock_fixB_addrs[] = {
    0x172FF1, 0x173094, // Method 28            C1+, C2+
    0x178C2D, 0x178D5C, // Method 20            C3, C4
    0x178F65, 0x179048, // Method 24            C5, C6
    0x1791D3, 0x179259, // Method 10            C7+, C8+
    0x17D14C, // Handle packet 11               C9+
    0x17D2DB, // Handle packet 12               C9+
    0x17D37F, 0x17D3ED, // Handle packet 13     C10+, C8+
    0x17D5C7, // Handle packet 15               C8+
    0x17D69F, 0x17D706, // Handle packet 16     C11, C8+
};

// CC, F5000000, NOP3
static constexpr uint8_t unlock_fixB1[] = {
    0x8B, 0x4D, 0xCC,                           // MOV ECX, DWORD PTR [EBP-0x34]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x0F, 0x85, 0xF5, 0x00, 0x00, 0x00,         // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP3
};

// CC, 32, NOP3
static constexpr uint8_t unlock_fixB2[] = {
    0x8B, 0x4D, 0xCC,                           // MOV ECX, DWORD PTR [EBP-0x34]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x75, 0x32,                                 // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP3
};

// E0, 24020000
static constexpr uint8_t unlock_fixB3[] = {
    0x8B, 0x4D, 0xE0,                           // MOV ECX, DWORD PTR [EBP-0x20]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x0F, 0x85, 0x24, 0x02, 0x00, 0x00,         // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
};

// E0, 2F
static constexpr uint8_t unlock_fixB4[] = {
    0x8B, 0x4D, 0xE0,                           // MOV ECX, DWORD PTR [EBP-0x20]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x75, 0x2F,                                 // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
};

// E8, D8010000
static constexpr uint8_t unlock_fixB5[] = {
    0x8B, 0x4D, 0xE8,                           // MOV ECX, DWORD PTR [EBP-0x18]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x0F, 0x85, 0xD8, 0x01, 0x00, 0x00,         // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
};

// E8, 2F
static constexpr uint8_t unlock_fixB6[] = {
    0x8B, 0x4D, 0xE8,                           // MOV ECX, DWORD PTR [EBP-0x18]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x75, 0x2F,                                 // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
};

// EC, DB000000, NOP3
static constexpr uint8_t unlock_fixB7[] = {
    0x8B, 0x4D, 0xEC,                           // MOV ECX, DWORD PTR [EBP-0x14]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x0F, 0x85, 0xDB, 0x00, 0x00, 0x00,         // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP3
};

// EC, 32, NOP3
static constexpr uint8_t unlock_fixB8[] = {
    0x8B, 0x4D, 0xEC,                           // MOV ECX, DWORD PTR [EBP-0x14]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x75, 0x32,                                 // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP3
};

// E8, 32, NOP3
static constexpr uint8_t unlock_fixB9[] = {
    0x8B, 0x4D, 0xE8,                           // MOV ECX, DWORD PTR [EBP-0x18]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x75, 0x32,                                 // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP3
};

// EC, D4010000, NOP3
static constexpr uint8_t unlock_fixB10[] = {
    0x8B, 0x4D, 0xEC,                           // MOV ECX, DWORD PTR [EBP-0x14]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x0F, 0x85, 0xD4, 0x01, 0x00, 0x00,         // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP3
};

// EC, AC000000, NOP2
static constexpr uint8_t unlock_fixB11[] = {
    0x8B, 0x4D, 0xEC,                           // MOV ECX, DWORD PTR [EBP-0x14]
    0xFE, 0x49, 0x8F,                           // DEC BYTE PTR [ECX-0x71]
    0x0F, 0x85, 0xAC, 0x00, 0x00, 0x00,         // JNZ
    0x31, 0xD2,                                 // XOR EDX, EDX
    0x87, 0x51, 0xB0,                           // XCHG DWORD PTR [ECX-0x50], EDX
    BASE_NOP2
};

#endif

void patch_netplay() {
    mem_write(patchA_addr, PATCH_BYTES<INT8_MAX>);

#if BETTER_BLACK_SCREEN_FIX
    //mem_write(0x171F66_R, PATCH_BYTES<0x1E>);
    mem_write(0x17C6E6_R, PATCH_BYTES<0x1E>);
    mem_write(0x17C6FB_R, PATCH_BYTES<0x1E>);
    mem_write(0x17C945_R, PATCH_BYTES<0x1E>);
    mem_write(0x171F4B_R, NOP_BYTES(1));
    mem_write(0x171F64_R, PATCH_BYTES<0x89>);
#endif

    resync_patch(160);

    // This may seem redundant, but it helps prevent
    // conflicts with the original netplay patch
    hotpatch_import(wsarecvfrom_import_addr, WSARecvFrom);


    hotpatch_rel32(0x176B8A_R, packet_parser_hook);
    hotpatch_import(wsasendto_import_addr, my_WSASendTo);


#if BETTER_BLACK_SCREEN_FIX
    mem_write(0x172FF1_R, unlock_fixB1);
    mem_write(0x173094_R, unlock_fixB2);
    mem_write(0x178C2D_R, unlock_fixB3);
    mem_write(0x178D5C_R, unlock_fixB4);
    mem_write(0x178F65_R, unlock_fixB5);
    mem_write(0x179048_R, unlock_fixB6);
    mem_write(0x1791D3_R, unlock_fixB7);
    mem_write(0x179259_R, unlock_fixB8);
    mem_write(0x17D14C_R, unlock_fixB9);
    mem_write(0x17D2DB_R, unlock_fixB9);
    mem_write(0x17D37F_R, unlock_fixB10);
    mem_write(0x17D3ED_R, unlock_fixB8);
    mem_write(0x17D5C7_R, unlock_fixB8);
    mem_write(0x17D69F_R, unlock_fixB11);
    mem_write(0x17D706_R, unlock_fixB8);

    uintptr_t base = base_address;
    for (size_t i = 0; i < countof(lock_fix_addrs); ++i) {
        hotpatch_rel32(based_pointer(base, lock_fix_addrs[i]), fix_black_screen_lock);
    }
    for (size_t i = 0; i < countof(unlock_fixA_addrs); ++i) {
        hotpatch_rel32(based_pointer(base, unlock_fixA_addrs[i]), fix_black_screen_unlock);
    }
#endif

    /*
    if (get_ipv6_enabled()) {
        mem_write(0x172AAF_R, AF_INET6);
    }
    */
}

