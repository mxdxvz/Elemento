/*
 *  ELEMENTO – Client
 *  Compile:  gcc -o client client.c
 *  Run:      ./client <server_ip>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "elemento.h"

// ════════════════════════════════════════════════════════════════
//  ANSI colours & box-drawing
// ════════════════════════════════════════════════════════════════
#define RED     "\033[1;31m"
#define YEL     "\033[1;33m"
#define GRN     "\033[1;32m"
#define CYN     "\033[1;36m"
#define BLU     "\033[1;34m"
#define MAG     "\033[1;35m"
#define WHT     "\033[1;37m"
#define ORG     "\033[38;5;208m"
#define RST     "\033[0m"

// Element colour lookup
static const char *ELEM_CLR[] = { RED, BLU, YEL, CYN };
static const char *ELEM_EMOJI[] = { "🔥", "💧", "🪨", "🌪 " };

// ════════════════════════════════════════════════════════════════
//  Globals
// ════════════════════════════════════════════════════════════════
static int      sock_fd;
static int      my_turn     = 0;   // set by MSG_YOUR_TURN / MSG_LOBBY
static int      game_active = 1;
static int      in_lobby    = 1;   // 1 until MSG_READY received
static GameState gs;               // last known state
static int      my_idx = -1;      // 0 or 1, set after MSG_READY

// ════════════════════════════════════════════════════════════════
//  HP bar renderer
// ════════════════════════════════════════════════════════════════
static void print_hp_bar(const char *name, int hp, int max_hp,
                         const char *elem_clr, const char *emoji) {
    int bar_width = 20;
    int filled = (max_hp > 0) ? (hp * bar_width / max_hp) : 0;
    if (filled < 0) filled = 0;
    if (filled > bar_width) filled = bar_width;

    printf("%s%s %-14s%s HP: %s", elem_clr, emoji, name, RST, GRN);
    for (int i = 0; i < filled;      i++) printf("█");
    printf(RED);
    for (int i = filled; i < bar_width; i++) printf("░");
    printf(RST " %s%d/%d%s\n", WHT, hp, max_hp, RST);
}

// ════════════════════════════════════════════════════════════════
//  Parse and display state
// ════════════════════════════════════════════════════════════════
static void parse_state(const char *buf) {
    // Format: name0|hp0|maxhp0|elem0|burn0|shield0|skip0 name1|... turn stage round
    int elem0 = 0, elem1 = 0;
    int r = sscanf(buf,
        "%31[^|]|%d|%d|%d|%d|%d|%d "
        "%31[^|]|%d|%d|%d|%d|%d|%d "
        "%d %d %d",
        gs.p[0].name, &gs.p[0].hp, &gs.p[0].max_hp, &elem0,
        &gs.p[0].burn_turns, &gs.p[0].shield, &gs.p[0].skip_turn,
        gs.p[1].name, &gs.p[1].hp, &gs.p[1].max_hp, &elem1,
        &gs.p[1].burn_turns, &gs.p[1].shield, &gs.p[1].skip_turn,
        &gs.turn, &gs.stage, &gs.round);

    if (r != 17) {
        printf("[warn] parse_state: sscanf got %d/17 fields\n", r);
        return;
    }
    gs.p[0].element = (Element)elem0;
    gs.p[1].element = (Element)elem1;

    printf("\n" CYN "╔══════════════════════════════════════════════╗\n");
    printf("║  ELEMENTO  │  Stage: %d  │  Round: %-4d       ║\n",
           gs.stage, gs.round);
    printf("╚══════════════════════════════════════════════╝" RST "\n");

    for (int i = 0; i < 2; i++) {
        int ei = (int)gs.p[i].element;
        print_hp_bar(gs.p[i].name, gs.p[i].hp, gs.p[i].max_hp,
                     ELEM_CLR[ei], ELEM_EMOJI[ei]);
        // Status effects
        if (gs.p[i].shield > 0)
            printf("   🛡  Shield: %d   ", gs.p[i].shield);
        if (gs.p[i].burn_turns > 0)
            printf("   🔥 Burning: %d turns   ", gs.p[i].burn_turns);
        if (gs.p[i].skip_turn > 0)
            printf("   ❄  Frozen!");
        if (gs.p[i].shield > 0 || gs.p[i].burn_turns > 0 || gs.p[i].skip_turn > 0)
            printf("\n");
    }
    printf("\n");
}

// ════════════════════════════════════════════════════════════════
//  Display element menu
// ════════════════════════════════════════════════════════════════
static void show_element_menu(void) {
    printf(WHT
        "╔══════════════════════════════════════╗\n"
        "║        Choose Your Element           ║\n"
        "╠══════════════════════════════════════╣\n"
        "║  " RED  "1) 🔥 Apoy   (Siklab / Fire)   " WHT "    ║\n"
        "║  " BLU  "2) 💧 Tubig  (Agos  / Water)  " WHT "     ║\n"
        "║  " YEL  "3) 🪨 Lupa   (Earth)           " WHT "    ║\n"
        "║  " CYN  "4) 🌪  Hangin (Hagupit / Air)  " WHT "     ║\n"
        "╠══════════════════════════════════════╣\n"
        "║  " MAG  "c) 💬 Chat                    " WHT "     ║\n"
        "╚══════════════════════════════════════╝\n" RST);
    printf("Your choice: ");
    fflush(stdout);
}

static int      banner_shown = 0;  // print banner only once

// ════════════════════════════════════════════════════════════════
//  Rainbow ELEMENTO banner – each letter its own colour
// ════════════════════════════════════════════════════════════════
static void print_banner(void) {
    if (banner_shown) return;
    banner_shown = 1;

    // E=Red  L=Orange  E=Yellow  M=Green  E=Blue  N=Cyan  T=Magenta  O=Pink
    #define _R  "\033[1;31m"        // E – Red
    #define _O  "\033[38;5;208m"    // L – Orange
    #define _Y  "\033[1;33m"        // E – Yellow
    #define _G  "\033[1;32m"        // M – Green
    #define _B  "\033[1;34m"        // E – Blue
    #define _C  "\033[1;36m"        // N – Cyan
    #define _M  "\033[1;35m"        // T – Magenta
    #define _P  "\033[38;5;213m"    // O – Pink

    printf("\n");
    printf(WHT "╔══════════════════════════════════════════════════════════════════════════════╗\n" RST);

    // Row 1
    printf("  "
        _R  "███████╗" _O "██╗     " _Y "███████╗" _G "███╗   ███╗" _B "███████╗" _C "███╗   ██╗" _M "████████╗" _P " ██████╗ "
        RST "\n");
    // Row 2
    printf("  "
        _R  "██╔════╝" _O "██║     " _Y "██╔════╝" _G "████╗ ████║" _B "██╔════╝" _C "████╗  ██║" _M "╚══██╔══╝" _P "██╔═══██╗"
        RST "\n");
    // Row 3
    printf("  "
        _R  "█████╗  " _O "██║     " _Y "█████╗  " _G "██╔████╔██║" _B "█████╗  " _C "██╔██╗ ██║" _M "   ██║   " _P "██║   ██║"
        RST "\n");
    // Row 4
    printf("  "
        _R  "██╔══╝  " _O "██║     " _Y "██╔══╝  " _G "██║╚██╔╝██║" _B "██╔══╝  " _C "██║╚██╗██║" _M "   ██║   " _P "██║   ██║"
        RST "\n");
    // Row 5
    printf("  "
        _R  "███████╗" _O "███████╗" _Y "███████╗" _G "██║ ╚═╝ ██║" _B "███████╗" _C "██║ ╚████║" _M "   ██║   " _P "╚██████╔╝"
        RST "\n");
    // Row 6
    printf("  "
        _R  "╚══════╝" _O "╚══════╝" _Y "╚══════╝" _G "╚═╝     ╚═╝" _B "╚══════╝" _C "╚═╝  ╚═══╝" _M "   ╚═╝   " _P " ╚═════╝ "
        RST "\n");

    printf(WHT "╚══════════════════════════════════════════════════════════════════════════════╝\n" RST);

    #undef _R
    #undef _O
    #undef _Y
    #undef _G
    #undef _B
    #undef _C
    #undef _M
    #undef _P

    printf("\n   "
        "\033[1;31m🔥 Apoy  "
        "\033[1;34m💧 Tubig  "
        "\033[1;33m🪨 Lupa  "
        "\033[1;36m🌪  Hangin"
        RST "\n");
    printf(WHT "   ══════════════════════════════════════\n" RST);
    printf(YEL "        An Elemental Battle Dice Game\n" RST);
    printf(WHT "   ══════════════════════════════════════\n\n" RST);
}

// ════════════════════════════════════════════════════════════════
//  Receiver thread – continuously reads server messages
// ════════════════════════════════════════════════════════════════
static void *recv_thread(void *arg) {
    (void)arg;
    Packet pkt;

    while (game_active) {
        int n = recv_packet(sock_fd, &pkt);
        if (n <= 0) {
            if (game_active) printf(RED "\nDisconnected from server.\n" RST);
            game_active = 0;
            break;
        }

        switch (pkt.type) {
            case MSG_READY:
                in_lobby = 0;
                my_turn  = 0;
                printf(GRN "\n%s\n" RST, pkt.payload);
                break;

            case MSG_LOBBY_WAIT:
                print_banner();
                printf(YEL "\n⏳ %s\n" RST, pkt.payload);
                fflush(stdout);
                break;

            case MSG_LOBBY:
                // Server is sending lobby content/prompt to host – print it and set flag
                print_banner();
                printf(MAG "\n%s" RST, pkt.payload);
                fflush(stdout);
                my_turn = 1;   // reuse my_turn so input loop picks it up
                break;

            case MSG_ROLL:
                printf("%s\n", pkt.payload);
                fflush(stdout);
                break;

            case MSG_STATE:
                parse_state(pkt.payload);
                fflush(stdout);
                break;

            case MSG_YOUR_TURN:
                my_turn = 1;
                printf(GRN "\n▶  %s\n" RST, pkt.payload);
                fflush(stdout);
                break;

            case MSG_WAIT_TURN:
                my_turn = 0;
                printf(YEL "\n⏳ %s\n" RST, pkt.payload);
                fflush(stdout);
                break;

            case MSG_STAGE_UP:
                printf(MAG "%s\n" RST, pkt.payload);
                fflush(stdout);
                break;

            case MSG_GAMEOVER:
                printf(MAG "\n%s\n" RST, pkt.payload);
                game_active = 0;
                my_turn = 0;
                fflush(stdout);
                break;

            case MSG_CHAT:
                printf(CYN "\n💬 %s\n" RST, pkt.payload);
                fflush(stdout);
                break;

            default:
                break;
        }
    }
    return NULL;
}

// ════════════════════════════════════════════════════════════════
//  Main
// ════════════════════════════════════════════════════════════════
int main(int argc, char *argv[]) {
    const char *server_ip = (argc > 1) ? argv[1] : "127.0.0.1";

    printf(CYN
        "╔══════════════════════════════════════╗\n"
        "║   ELEMENTO  –  CLIENT  (v1.0)        ║\n"
        "╚══════════════════════════════════════╝\n" RST);

    // ── Connect ─────────────────────────────────────────────────
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", server_ip);
        exit(1);
    }

    printf("Connecting to %s:%d …\n", server_ip, PORT);
    if (connect(sock_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("connect"); exit(1);
    }
    printf(GRN "Connected!\n" RST);

    // ── Send player name ─────────────────────────────────────────
    char name[32];
    printf("Enter your warrior name: ");
    fflush(stdout);
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = '\0';
    if (strlen(name) == 0) strcpy(name, "Warrior");

    Packet pkt;
    pkt.type = MSG_HELLO;
    strncpy(pkt.payload, name, sizeof(pkt.payload)-1);
    send_packet(sock_fd, &pkt);

    // ── Spawn receiver thread ────────────────────────────────────
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);

    // ── Input loop ───────────────────────────────────────────────
    while (game_active) {
        if (!my_turn) {
            usleep(100000); // 100ms poll
            continue;
        }

        // ── Lobby mode (host only) ───────────────────────────────
        if (in_lobby) {
            char line[64];
            if (!fgets(line, sizeof(line), stdin)) break;
            line[strcspn(line, "\n")] = '\0';
            if (!my_turn) continue;
            my_turn = 0;

            pkt.type = MSG_LOBBY_CHOICE;
            strncpy(pkt.payload, line, sizeof(pkt.payload)-1);
            send_packet(sock_fd, &pkt);
            continue;
        }

        // ── Game mode ────────────────────────────────────────────
        show_element_menu();

        char line[64];
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';

        if (!my_turn) continue; // turn stolen while reading

        if (line[0] == 'c' || line[0] == 'C') {
            // Chat mode
            printf("Chat message: ");
            fflush(stdout);
            char chat[BUFSIZE-64];
            if (!fgets(chat, sizeof(chat), stdin)) continue;
            chat[strcspn(chat, "\n")] = '\0';
            pkt.type = MSG_CHAT;
            strncpy(pkt.payload, chat, sizeof(pkt.payload)-1);
            send_packet(sock_fd, &pkt);
            continue;
        }

        int choice = atoi(line);
        if (choice < 1 || choice > 4) {
            printf(RED "Invalid choice. Enter 1-4 (or c for chat).\n" RST);
            continue;
        }

        // ── Send action ─────────────────────────────────────────
        pkt.type = MSG_ACTION;
        snprintf(pkt.payload, sizeof(pkt.payload), "%d", choice);
        send_packet(sock_fd, &pkt);
        my_turn = 0;
    }

    pthread_join(tid, NULL);
    close(sock_fd);

    printf(CYN "Thanks for playing ELEMENTO!\n" RST);
    return 0;
}