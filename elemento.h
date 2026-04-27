#ifndef ELEMENTO_H
#define ELEMENTO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ─── Network ───────────────────────────────────────────────────────────────
#define PORT        9090
#define BUFSIZE     4096

// ─── Game constants ────────────────────────────────────────────────────────
#define MAX_HP      100
#define EARTH_BONUS_HP 20
#define BURN_DMG    3
#define SHIELD_VAL  8
#define HEAL_VAL    10
#define ADV_MULT    1.5   // +50% elemental advantage

// ─── Elements ──────────────────────────────────────────────────────────────
typedef enum {
    APOY    = 0,   // Fire  – Siklab
    TUBIG   = 1,   // Water – Agos
    LUPA    = 2,   // Earth – Lupa
    HANGIN  = 3    // Air   – Hagupit
} Element;

static const char *ELEM_NAME[]  = {"Apoy (Siklab)", "Tubig (Agos)",
                                    "Lupa",          "Hangin (Hagupit)"};
static const char *ELEM_SHORT[] = {"APOY","TUBIG","LUPA","HANGIN"};

// ─── Message types (sent over TCP) ─────────────────────────────────────────
typedef enum {
    MSG_HELLO        = 1,   // client → server: player name
    MSG_READY        = 2,   // server → client: game starts, sends stage
    MSG_YOUR_TURN    = 3,   // server → client: it's your turn
    MSG_WAIT_TURN    = 4,   // server → client: wait
    MSG_ACTION       = 5,   // client → server: element choice
    MSG_ROLL         = 6,   // server → both:  dice result + effects narrative
    MSG_STATE        = 7,   // server → both:  full game state snapshot
    MSG_GAMEOVER     = 8,   // server → both:  winner announcement
    MSG_STAGE_UP     = 9,   // server → both:  stage buff activated
    MSG_CHAT         = 10,  // either direction: chat line
    MSG_LOBBY        = 11,  // server → host: show lobby menu
    MSG_LOBBY_CHOICE = 12,  // host → server: lobby menu selection
    MSG_LOBBY_WAIT   = 13   // server → guest: waiting for host
} MsgType;

// ─── Packed network packet ─────────────────────────────────────────────────
typedef struct {
    MsgType type;
    char    payload[BUFSIZE - sizeof(MsgType)];
} Packet;

// ─── Per-player state ──────────────────────────────────────────────────────
typedef struct {
    char    name[32];
    int     hp;
    int     max_hp;
    Element element;
    int     burn_turns;    // remaining burn-damage turns
    int     shield;        // current shield HP
    int     skip_turn;     // turns to skip (freeze / Nature's Revenge)
    int     extra_die;     // extra die from Air buff
} Player;

// ─── Full game state (server is authoritative) ─────────────────────────────
typedef struct {
    Player  p[2];          // p[0]=server-side player, p[1]=client-side player
    int     turn;          // whose turn: 0 or 1
    int     stage;         // 1-4
    int     stage_buff;    // which element buff is active (-1 = none)
    int     round;
} GameState;

// ─── Helpers ───────────────────────────────────────────────────────────────
static inline int roll_die(void) { return (rand() % 6) + 1; }

// Returns 1 if attacker has elemental advantage over defender
// Apoy > Hangin > Lupa > Tubig > Apoy
static inline int has_advantage(Element atk, Element def) {
    return (atk == APOY   && def == HANGIN) ||
           (atk == HANGIN && def == LUPA)   ||
           (atk == LUPA   && def == TUBIG)  ||
           (atk == TUBIG  && def == APOY);
}

// Send / recv helpers
static inline int send_packet(int fd, const Packet *p) {
    size_t total = 0;
    const char *buf = (const char *)p;
    while (total < sizeof(Packet)) {
        int n = send(fd, buf + total, sizeof(Packet) - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return (int)total;
}
static inline int recv_packet(int fd, Packet *p) {
    size_t total = 0;
    char *buf = (char *)p;
    while (total < sizeof(Packet)) {
        int n = recv(fd, buf + total, sizeof(Packet) - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return (int)total;
}

#endif // ELEMENTO_H