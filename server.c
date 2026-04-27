/*
 *  ELEMENTO – Server
 *  Compile:  gcc -o server server.c -lm
 *  Run:      ./server <port_number>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "elemento.h"

// ════════════════════════════════════════════════════════════════
//  ANSI colours
// ════════════════════════════════════════════════════════════════
#define RED     "\033[1;31m"
#define YEL     "\033[1;33m"
#define GRN     "\033[1;32m"
#define CYN     "\033[1;36m"
#define BLU     "\033[1;34m"
#define MAG     "\033[1;35m"
#define WHT     "\033[1;37m"
#define RST     "\033[0m"

// ════════════════════════════════════════════════════════════════
//  Global server state
// ════════════════════════════════════════════════════════════════
static int       client_fd[2];   // 0=server-player, 1=client-player
static GameState gs;

// ════════════════════════════════════════════════════════════════
//  Utility: broadcast to both players
// ════════════════════════════════════════════════════════════════
static void broadcast(const Packet *pkt) {
    for (int i = 0; i < 2; i++)
        send_packet(client_fd[i], pkt);
}

static void broadcast_str(MsgType t, const char *msg) {
    Packet pkt;
    pkt.type = t;
    strncpy(pkt.payload, msg, sizeof(pkt.payload)-1);
    pkt.payload[sizeof(pkt.payload)-1] = '\0';
    broadcast(&pkt);
}

static void send_str(int idx, MsgType t, const char *msg) {
    Packet pkt;
    pkt.type = t;
    strncpy(pkt.payload, msg, sizeof(pkt.payload)-1);
    pkt.payload[sizeof(pkt.payload)-1] = '\0';
    send_packet(client_fd[idx], &pkt);
}

// ════════════════════════════════════════════════════════════════
//  Serialise GameState into a string and broadcast
// ════════════════════════════════════════════════════════════════
static void broadcast_state(void) {
    char buf[BUFSIZE];
    // Format: name0|hp0|maxhp0|elem0|burn0|shield0|skip0 name1|hp1|maxhp1|elem1|burn1|shield1|skip1 turn stage round
    snprintf(buf, sizeof(buf),
        "%s|%d|%d|%d|%d|%d|%d "
        "%s|%d|%d|%d|%d|%d|%d "
        "%d %d %d",
        gs.p[0].name, gs.p[0].hp, gs.p[0].max_hp, (int)gs.p[0].element,
        gs.p[0].burn_turns, gs.p[0].shield, gs.p[0].skip_turn,
        gs.p[1].name, gs.p[1].hp, gs.p[1].max_hp, (int)gs.p[1].element,
        gs.p[1].burn_turns, gs.p[1].shield, gs.p[1].skip_turn,
        gs.turn, gs.stage, gs.round);
    broadcast_str(MSG_STATE, buf);
}

// ════════════════════════════════════════════════════════════════
//  Stage / buff system
// ════════════════════════════════════════════════════════════════
static const char *STAGE_BUFF_DESC[] = {
    "STAGE 2 BUFF: Tubig (Water) Even rolls now FREEZE the enemy (skip turn)!",
    "STAGE 3 BUFF: Apoy (Fire) Burn damage is DOUBLED (6 dmg/turn)!",
    "STAGE 4 BUFF: Lupa (Earth) players start with +20 Max HP!",
    "STAGE 4 BUFF: Hangin (Air) gets an EXTRA die every turn!"
};

static void check_stage_up(void) {
    int new_stage = gs.stage;
    // Stage advances every 5 rounds
    if (gs.round >= 20) new_stage = 4;
    else if (gs.round >= 15) new_stage = 4; // all buffs active by stage 4
    else if (gs.round >= 10) new_stage = 3;
    else if (gs.round >= 5)  new_stage = 2;

    if (new_stage > gs.stage) {
        gs.stage = new_stage;
        gs.stage_buff = new_stage - 2; // buff index 0-based
        char msg[BUFSIZE];
        snprintf(msg, sizeof(msg),
            "\n★ ═══ STAGE %d REACHED ═══ ★\n%s\n",
            gs.stage,
            (gs.stage_buff >= 0 && gs.stage_buff < 4)
                ? STAGE_BUFF_DESC[gs.stage_buff] : "");
        broadcast_str(MSG_STAGE_UP, msg);
    }
}

// ════════════════════════════════════════════════════════════════
//  Core combat: process one attacker turn
//  Returns 1 if game over
// ════════════════════════════════════════════════════════════════
static int process_turn(int atk_idx) {
    int def_idx = 1 - atk_idx;
    Player *atk = &gs.p[atk_idx];
    Player *def = &gs.p[def_idx];

    char log[BUFSIZE*4];
    char tmp[256];
    log[0] = '\0';

    snprintf(tmp, sizeof(tmp),
        "\n══════════════════════════════════\n"
        "⚔  %s's Turn  [%s]\n"
        "══════════════════════════════════\n",
        atk->name, ELEM_SHORT[(int)atk->element]);
    strncat(log, tmp, sizeof(log)-strlen(log)-1);

    // ── Apply burn damage at start of turn ──────────────────────
    if (atk->burn_turns > 0) {
        int bd = (gs.stage_buff == 1) ? 6 : BURN_DMG; // stage3 doubles burn
        atk->hp -= bd;
        atk->burn_turns--;
        snprintf(tmp, sizeof(tmp), "🔥 %s takes %d burn damage! (%d turns left)\n",
            atk->name, bd, atk->burn_turns);
        strncat(log, tmp, sizeof(log)-strlen(log)-1);
        if (atk->hp <= 0) {
            broadcast_str(MSG_ROLL, log);
            return 1;
        }
    }

    // ── Skip turn (freeze / Nature's Revenge) ──────────────────
    if (atk->skip_turn > 0) {
        atk->skip_turn--;
        snprintf(tmp, sizeof(tmp), "❄  %s is stunned and skips this turn!\n", atk->name);
        strncat(log, tmp, sizeof(log)-strlen(log)-1);
        broadcast_str(MSG_ROLL, log);
        return 0;
    }

    // ── Roll dice ───────────────────────────────────────────────
    int num_dice = 3;
    // Air stage4 buff: always extra die
    if (atk->element == HANGIN && gs.stage_buff == 3) {
        atk->extra_die = 1;
    }

    int dice[5];
    int total = 0;
    for (int i = 0; i < num_dice; i++) {
        dice[i] = roll_die();
        total += dice[i];
    }

    // ── Triple Rule – Ganti ng Kalikasan ────────────────────────
    int triple = (dice[0] == dice[1] && dice[1] == dice[2]);

    snprintf(tmp, sizeof(tmp), "🎲 Dice: [%d][%d][%d] → Sum: %d\n",
        dice[0], dice[1], dice[2], total);
    strncat(log, tmp, sizeof(log)-strlen(log)-1);

    if (triple) {
        strncat(log,
            "🌟 ╔══════════════════════════════╗\n"
            "   ║  GANTI NG KALIKASAN!!        ║\n"
            "   ╚══════════════════════════════╝\n",
            sizeof(log)-strlen(log)-1);
        int dmg = total * 3;
        // Apply shield
        int absorbed = (def->shield >= dmg) ? dmg : def->shield;
        def->shield -= absorbed;
        dmg -= absorbed;
        def->hp -= dmg;
        def->skip_turn = 1;
        snprintf(tmp, sizeof(tmp),
            "💥 Nature's Revenge: %d × 3 = %d DMG on %s!\n"
            "😵 %s skips their next turn!\n",
            total, total*3, def->name, def->name);
        strncat(log, tmp, sizeof(log)-strlen(log)-1);
        broadcast_str(MSG_ROLL, log);
        return (def->hp <= 0);
    }

    // ── Elemental advantage ─────────────────────────────────────
    int advantage = has_advantage(atk->element, def->element);
    if (advantage) {
        strncat(log, "⚡ Elemental Advantage! (+50% damage)\n",
            sizeof(log)-strlen(log)-1);
    }

    int is_even = (total % 2 == 0);
    int base_dmg = total;
    if (advantage) base_dmg = (int)(base_dmg * ADV_MULT);

    // ── Element-specific effects ────────────────────────────────
    switch (atk->element) {

        case APOY:
            if (!is_even) {
                // Odd: straight damage
                int d = base_dmg;
                int absorbed = (def->shield >= d) ? d : def->shield;
                def->shield -= absorbed; d -= absorbed;
                def->hp -= d;
                snprintf(tmp, sizeof(tmp),
                    "🔥 Siklab ODD – Flame Strike: %d DMG on %s!\n",
                    base_dmg, def->name);
            } else {
                // Even: damage + burn
                int d = base_dmg;
                int absorbed = (def->shield >= d) ? d : def->shield;
                def->shield -= absorbed; d -= absorbed;
                def->hp -= d;
                def->burn_turns += 2;
                snprintf(tmp, sizeof(tmp),
                    "🔥 Siklab EVEN – Ember Curse: %d DMG + BURN (%d turns) on %s!\n",
                    base_dmg, def->burn_turns, def->name);
            }
            strncat(log, tmp, sizeof(log)-strlen(log)-1);
            break;

        case TUBIG:
            if (!is_even) {
                int d = base_dmg;
                int absorbed = (def->shield >= d) ? d : def->shield;
                def->shield -= absorbed; d -= absorbed;
                def->hp -= d;
                snprintf(tmp, sizeof(tmp),
                    "💧 Agos ODD – Tidal Surge: %d DMG on %s!\n",
                    base_dmg, def->name);
            } else {
                // Even: heal self (or freeze if stage2 buff)
                if (gs.stage_buff >= 0) { // stage 2+ – freeze
                    def->skip_turn = 1;
                    snprintf(tmp, sizeof(tmp),
                        "❄  Agos EVEN – Blizzard: %s is FROZEN (skips next turn)!\n",
                        def->name);
                } else {
                    atk->hp += HEAL_VAL;
                    if (atk->hp > atk->max_hp) atk->hp = atk->max_hp;
                    snprintf(tmp, sizeof(tmp),
                        "💧 Agos EVEN – Healing Tide: %s heals %d HP! (now %d/%d)\n",
                        atk->name, HEAL_VAL, atk->hp, atk->max_hp);
                }
            }
            strncat(log, tmp, sizeof(log)-strlen(log)-1);
            break;

        case LUPA:
            if (!is_even) {
                int d = base_dmg;
                int absorbed = (def->shield >= d) ? d : def->shield;
                def->shield -= absorbed; d -= absorbed;
                def->hp -= d;
                snprintf(tmp, sizeof(tmp),
                    "🪨 Lupa ODD – Boulder Crush: %d DMG on %s!\n",
                    base_dmg, def->name);
            } else {
                atk->shield += SHIELD_VAL;
                snprintf(tmp, sizeof(tmp),
                    "🛡  Lupa EVEN – Stone Ward: %s gains %d shield! (total: %d)\n",
                    atk->name, SHIELD_VAL, atk->shield);
            }
            strncat(log, tmp, sizeof(log)-strlen(log)-1);
            break;

        case HANGIN:
            if (!is_even) {
                int d = base_dmg;
                int absorbed = (def->shield >= d) ? d : def->shield;
                def->shield -= absorbed; d -= absorbed;
                def->hp -= d;
                snprintf(tmp, sizeof(tmp),
                    "🌪  Hagupit ODD – Wind Slash: %d DMG on %s!\n",
                    base_dmg, def->name);
            } else {
                // Even: extra die roll for bonus damage
                int bonus = roll_die();
                int bd = bonus;
                if (advantage) bd = (int)(bd * ADV_MULT);
                int absorbed = (def->shield >= bd) ? bd : def->shield;
                def->shield -= absorbed; bd -= absorbed;
                def->hp -= bd;
                // apply remaining damage to base
                int base_absorbed = (def->shield >= base_dmg) ? base_dmg : def->shield;
                def->shield -= base_absorbed;
                int remaining = base_dmg - base_absorbed;
                def->hp -= remaining;
                snprintf(tmp, sizeof(tmp),
                    "🌪  Hagupit EVEN – Gale Rush: %d DMG + bonus die [%d] = +%d DMG on %s!\n",
                    base_dmg, bonus, bd, def->name);
                // Correct the double-subtraction: we already applied base_dmg above with bonus
                // undo the separate remaining subtraction since base was applied with bonus
                def->hp += remaining; // fix: base_dmg was already applied once through bd path
            }
            strncat(log, tmp, sizeof(log)-strlen(log)-1);
            break;
    }

    // Clamp HP
    if (def->hp < 0) def->hp = 0;
    if (atk->hp < 0) atk->hp = 0;

    snprintf(tmp, sizeof(tmp),
        "── HP: %s %d/%d  │  %s %d/%d ──\n",
        atk->name, atk->hp, atk->max_hp,
        def->name, def->hp, def->max_hp);
    strncat(log, tmp, sizeof(log)-strlen(log)-1);

    broadcast_str(MSG_ROLL, log);
    return (def->hp <= 0 || atk->hp <= 0);
}

// ════════════════════════════════════════════════════════════════
//  Initialise / reset game
// ════════════════════════════════════════════════════════════════
static void init_game(void) {
    // Save names before zeroing
    char name0[32], name1[32];
    strncpy(name0, gs.p[0].name, 31); name0[31] = '\0';
    strncpy(name1, gs.p[1].name, 31); name1[31] = '\0';

    memset(&gs, 0, sizeof(gs));

    // Restore names
    strncpy(gs.p[0].name, name0, 31); gs.p[0].name[31] = '\0';
    strncpy(gs.p[1].name, name1, 31); gs.p[1].name[31] = '\0';

    gs.stage = 1;
    gs.stage_buff = -1;
    gs.turn = 0;
    gs.round = 1;

    for (int i = 0; i < 2; i++) {
        gs.p[i].max_hp = MAX_HP;
        gs.p[i].hp     = MAX_HP;
        gs.p[i].element = APOY;
        gs.p[i].shield  = 0;
        gs.p[i].burn_turns = 0;
        gs.p[i].skip_turn  = 0;
        gs.p[i].extra_die  = 0;
    }
}

// ════════════════════════════════════════════════════════════════
//  Main – setup & game loop
// ════════════════════════════════════════════════════════════════
int main(void) {
    srand((unsigned)time(NULL));

    int srv_fd;
    struct sockaddr_in addr;
    int opt = 1;

    printf(CYN
        "╔══════════════════════════════════════╗\n"
        "║   ELEMENTO  –  SERVER  (v1.0)        ║\n"
        "╚══════════════════════════════════════╝\n" RST);

    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) { perror("socket"); exit(1); }

    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    listen(srv_fd, 2);
    printf(GRN "Listening on port %d …\n" RST, PORT);

    // ── Accept two connections ───────────────────────────────────
    for (int i = 0; i < 2; i++) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        client_fd[i] = accept(srv_fd, (struct sockaddr*)&cli, &len);
        printf(GRN "Player %d connected from %s\n" RST,
            i+1, inet_ntoa(cli.sin_addr));
    }
    close(srv_fd);

    // ── Receive player names ────────────────────────────────────
    Packet pkt;
    for (int i = 0; i < 2; i++) {
        if (recv_packet(client_fd[i], &pkt) <= 0) {
            fprintf(stderr, "Failed to receive name from player %d\n", i+1);
            exit(1);
        }
        strncpy(gs.p[i].name, pkt.payload, 31);
        gs.p[i].name[31] = '\0';
        printf(YEL "Player %d name: %s\n" RST, i+1, gs.p[i].name);
    }

    // init_game() now preserves names internally before resetting stats
    init_game();

    // ── Notify guest to wait in lobby ───────────────────────────
    char guest_wait[BUFSIZE];
    snprintf(guest_wait, sizeof(guest_wait),
        "Both players connected!\n"
        "Waiting for host (%s) to start the game…\n",
        gs.p[0].name);
    send_str(1, MSG_LOBBY_WAIT, guest_wait);

    // ── Send lobby menu to host (player 0) ──────────────────────
    static const char *LOBBY_MENU =
        "╔══════════════════════════════════════════╗\n"
        "║                LOBBY (HOST)              ║\n"
        "╠══════════════════════════════════════════╣\n"
        "║  1) ⚔  Start Game                        ║\n"
        "║  2) 📖  How to Play / Game Mechanics     ║\n"
        "║  3) 🚪  Exit                             ║\n"
        "╚══════════════════════════════════════════╝\n"
        "Your choice: ";

    while (1) {
        send_str(0, MSG_LOBBY, LOBBY_MENU);

        if (recv_packet(client_fd[0], &pkt) <= 0) {
            fprintf(stderr, RED "Host disconnected in lobby.\n" RST);
            close(client_fd[0]); close(client_fd[1]);
            exit(0);
        }

        int lobby_choice = atoi(pkt.payload);

        if (lobby_choice == 1) {
            // ── Start game ───────────────────────────────────────
            break;

        } else if (lobby_choice == 2) {
            // ── Game mechanics ───────────────────────────────────
            static const char *MECHANICS =
                "\n╔══════════════════════════════════════════════════════╗\n"
                "║                HOW TO PLAY  ELEMENTO                ║\n"
                "╠══════════════════════════════════════════════════════╣\n"
                "║ • Each turn you pick an ELEMENT (Apoy/Tubig/Lupa/   ║\n"
                "║   Hangin). The server rolls 3 dice and applies its  ║\n"
                "║   effect based on odd/even sum.                     ║\n"
                "╠══════════════════════════════════════════════════════╣\n"
                "║ ELEMENTS & EFFECTS                                  ║\n"
                "║  🔥 Apoy (Fire)   ODD →  Flame Strike (damage)     ║\n"
                "║                   EVEN → Ember Curse (dmg + burn)  ║\n"
                "║  💧 Tubig (Water) ODD →  Tidal Surge (damage)      ║\n"
                "║                   EVEN → Healing Tide / Freeze     ║\n"
                "║  🪨 Lupa (Earth)  ODD →  Boulder Crush (damage)    ║\n"
                "║                   EVEN → Stone Ward (shield)       ║\n"
                "║  🌪  Hangin (Air)  ODD →  Wind Slash (damage)       ║\n"
                "║                   EVEN → Gale Rush (dmg + bonus)  ║\n"
                "╠══════════════════════════════════════════════════════╣\n"
                "║ ADVANTAGE CHAIN (deals +50%% damage):               ║\n"
                "║   Apoy > Hangin > Lupa > Tubig > Apoy              ║\n"
                "╠══════════════════════════════════════════════════════╣\n"
                "║ TRIPLE DICE  →  Ganti ng Kalikasan!                ║\n"
                "║   All 3 dice same → 3× damage + enemy skips turn   ║\n"
                "╠══════════════════════════════════════════════════════╣\n"
                "║ STAGES  (every 5 rounds a new buff activates):      ║\n"
                "║  Stage 2: Tubig even rolls FREEZE enemy            ║\n"
                "║  Stage 3: Apoy burn damage doubled                 ║\n"
                "║  Stage 4: Lupa +20 max HP / Hangin gets extra die  ║\n"
                "╚══════════════════════════════════════════════════════╝\n";
            send_str(0, MSG_LOBBY, MECHANICS);

        } else if (lobby_choice == 3) {
            // ── Exit ─────────────────────────────────────────────
            broadcast_str(MSG_GAMEOVER, "\n🚪 Host exited the lobby. Game cancelled.\n");
            printf(YEL "Host chose to exit. Shutting down.\n" RST);
            close(client_fd[0]); close(client_fd[1]);
            exit(0);

        } else {
            send_str(0, MSG_LOBBY, "Invalid choice. Enter 1, 2, or 3.\nYour choice: ");
        }
    }

    // ── Game is starting ────────────────────────────────────────
    char ready_msg[BUFSIZE];
    snprintf(ready_msg, sizeof(ready_msg),
        "⚔  GAME START!\nPlayer 1 (HOST): %s\nPlayer 2: %s\n"
        "May the elements favour you!\n",
        gs.p[0].name, gs.p[1].name);
    broadcast_str(MSG_READY, ready_msg);

    broadcast_state();

    // ════════════════════════════════════════════════════════════
    //  GAME LOOP
    // ════════════════════════════════════════════════════════════
    while (1) {
        int atk = gs.turn;
        int def = 1 - atk;

        // ── Notify whose turn ────────────────────────────────────
        send_str(atk, MSG_YOUR_TURN,
            "Your turn! \n");
        send_str(def, MSG_WAIT_TURN,
            "Waiting for opponent to choose…\n");

        // ── Receive element choice from active player ────────────
        if (recv_packet(client_fd[atk], &pkt) <= 0) {
            printf(RED "Player %d disconnected.\n" RST, atk+1);
            break;
        }
        if (pkt.type == MSG_CHAT) {
            // relay chat to other player
            char chat_buf[BUFSIZE];
            snprintf(chat_buf, sizeof(chat_buf), "[%s]: %s",
                gs.p[atk].name, pkt.payload);
            send_str(def, MSG_CHAT, chat_buf);
            // re-poll (simple: just loop again — for production use select())
            if (recv_packet(client_fd[atk], &pkt) <= 0) break;
        }

        int choice = atoi(pkt.payload);
        if (choice < 1 || choice > 4) choice = 1;
        gs.p[atk].element = (Element)(choice - 1);

        // Apply Lupa stage4 HP buff if switching to Lupa in stage 4
        if (gs.p[atk].element == LUPA && gs.stage >= 4) {
            if (gs.p[atk].max_hp == MAX_HP) {
                gs.p[atk].max_hp += EARTH_BONUS_HP;
                if (gs.p[atk].hp == MAX_HP) gs.p[atk].hp = gs.p[atk].max_hp;
            }
        }

        // Announce element choice
        char choice_msg[BUFSIZE];
        snprintf(choice_msg, sizeof(choice_msg),
            "⚔  %s chose %s!\n",
            gs.p[atk].name, ELEM_NAME[(int)gs.p[atk].element]);
        broadcast_str(MSG_ROLL, choice_msg);

        // ── Process turn ─────────────────────────────────────────
        int game_over = process_turn(atk);
        broadcast_state();

        if (game_over) {
            char winner[BUFSIZE];
            // Determine winner
            int w = (gs.p[0].hp > 0) ? 0 : 1;
            if (gs.p[0].hp <= 0 && gs.p[1].hp <= 0) {
                snprintf(winner, sizeof(winner),
                    "\n🌟 DRAW! Both warriors fall!\n");
            } else {
                snprintf(winner, sizeof(winner),
                    "\n🏆 ══════════════════════════════ 🏆\n"
                    "  WINNER: %s\n"
                    "  (%s has been defeated!)\n"
                    "🏆 ══════════════════════════════ 🏆\n",
                    gs.p[w].name, gs.p[1-w].name);
            }
            broadcast_str(MSG_GAMEOVER, winner);
            printf(MAG "%s" RST, winner);
            break;
        }

        // ── Advance turn / round / stage ─────────────────────────
        gs.turn = def;
        if (gs.turn == 0) gs.round++;
        check_stage_up();
    }

    printf(CYN "Server shutting down.\n" RST);
    close(client_fd[0]);
    close(client_fd[1]);
    return 0;
}