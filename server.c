#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>

#define PORT 2026
#define MAX_CLIENTS 10
#define MAX_GAMES 4
#define MAX_USERS 100
#define MAX_TURNS 10          // 10 turnos por jugador
#define BOARD_SIZE 5

typedef struct {
    char user[50];
    char pass[50];
} User;

typedef struct {
    int fd;
    int in_use;
    int authenticated;
    char pending_user[50];
    char user[50];
    int game_id;
    int player_num;           // 0 o 1
    char inbuf[2048];
    int inlen;
} Client;

typedef struct {
    int active;
    int clients[2];           // índices en clients[]
    int turn;                 // 0 o 1
    int turn_count;           // acciones realizadas
    char board[BOARD_SIZE][BOARD_SIZE]; // 'N', '1', '2'
    int units[BOARD_SIZE][BOARD_SIZE];
} Game;

Client clients[MAX_CLIENTS];
Game games[MAX_GAMES];
int waiting_client = -1;
User users[MAX_USERS];
int num_users = 0;

/* Prototipos */
void send_msg(int fd, const char *fmt, ...);
int find_user(const char *user);
void add_user(const char *user, const char *pass);
int parse_territory(const char *s, int *r, int *c);
char player_char(int p);
int count_territories(int game_id, int player);
int total_units(int game_id, int player);
void send_state(int client_idx);
void start_game(int idx1, int idx2);
void end_game(int game_id, const char *reason);
void next_turn(int game_id);
void handle_read(int idx);
void process_command(int idx, char *line);
void disconnect_client(int idx);
void handle_iniciar(int idx);
void handle_conquistar(int idx, char *orig, char *dest, int units);
void handle_atacar(int idx, char *orig, char *dest, int units);
void handle_reforzar(int idx, char *orig, char *dest, int units);
void handle_pasar(int idx);

/* Envía un mensaje formateado terminado en '\n' */
void send_msg(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
    send(fd, buf, strlen(buf), 0);
}

int find_user(const char *user) {
    for (int i = 0; i < num_users; i++)
        if (strcmp(users[i].user, user) == 0) return i;
    return -1;
}

void add_user(const char *user, const char *pass) {
    if (num_users >= MAX_USERS) return;
    strcpy(users[num_users].user, user);
    strcpy(users[num_users].pass, pass);
    num_users++;
}

int parse_territory(const char *s, int *r, int *c) {
    if (strlen(s) != 2) return 0;
    char row = toupper(s[0]);
    char col = s[1];
    if (row < 'A' || row > 'E') return 0;
    if (col < '1' || col > '5') return 0;
    *r = row - 'A';
    *c = col - '1';
    return 1;
}

char player_char(int p) {
    return p == 0 ? '1' : '2';
}

int count_territories(int game_id, int player) {
    Game *g = &games[game_id];
    char pc = player_char(player);
    int count = 0;
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int j = 0; j < BOARD_SIZE; j++)
            if (g->board[i][j] == pc) count++;
    return count;
}

int total_units(int game_id, int player) {
    Game *g = &games[game_id];
    char pc = player_char(player);
    int sum = 0;
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int j = 0; j < BOARD_SIZE; j++)
            if (g->board[i][j] == pc) sum += g->units[i][j];
    return sum;
}

void send_state(int client_idx) {
    Client *c = &clients[client_idx];
    if (c->game_id < 0) {
        send_msg(c->fd, "-Err. No estás en una partida");
        return;
    }
    Game *g = &games[c->game_id];
    int me = c->player_num;
    send_msg(c->fd, "+Ok. Estado del tablero:");
    send_msg(c->fd, "  1 2 3 4 5");
    for (int i = 0; i < BOARD_SIZE; i++) {
        char row[256];
        int pos = sprintf(row, "%c", 'A' + i);
        for (int j = 0; j < BOARD_SIZE; j++) {
            char cell[32];
            if (g->board[i][j] == 'N') {
                sprintf(cell, " .");
            } else if (g->board[i][j] == player_char(me)) {
                sprintf(cell, " J%d(%d)", me + 1, g->units[i][j]);
            } else {
                sprintf(cell, " J%d", (g->board[i][j] == '1' ? 1 : 2));
            }
            pos += sprintf(row + pos, " %s", cell);
        }
        send_msg(c->fd, "%s", row);
    }
}

void start_game(int idx1, int idx2) {
    int game_id = -1;
    for (int i = 0; i < MAX_GAMES; i++) {
        if (!games[i].active) { game_id = i; break; }
    }
    if (game_id < 0) {
        send_msg(clients[idx1].fd, "-Err. No hay partidas disponibles");
        send_msg(clients[idx2].fd, "-Err. No hay partidas disponibles");
        return;
    }
    Game *g = &games[game_id];
    g->active = 1;
    g->clients[0] = idx1;
    g->clients[1] = idx2;
    g->turn = rand() % 2;
    g->turn_count = 0;
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int j = 0; j < BOARD_SIZE; j++) {
            g->board[i][j] = 'N';
            g->units[i][j] = 0;
        }
    int r1, c1, r2, c2;
    do {
        r1 = rand() % BOARD_SIZE; c1 = rand() % BOARD_SIZE;
        r2 = rand() % BOARD_SIZE; c2 = rand() % BOARD_SIZE;
    } while (r1 == r2 && c1 == c2);
    g->board[r1][c1] = '1'; g->units[r1][c1] = 30;
    g->board[r2][c2] = '2'; g->units[r2][c2] = 30;
    clients[idx1].game_id = game_id;
    clients[idx1].player_num = 0;
    clients[idx2].game_id = game_id;
    clients[idx2].player_num = 1;
    send_msg(clients[idx1].fd, "+Ok. Empieza la partida");
    send_msg(clients[idx2].fd, "+Ok. Empieza la partida");
    send_state(idx1);
    send_state(idx2);
    if (g->turn == 0) {
        send_msg(clients[idx1].fd, "+Ok. Es tu turno");
        send_msg(clients[idx2].fd, "+Ok. Espera tu turno");
    } else {
        send_msg(clients[idx1].fd, "+Ok. Espera tu turno");
        send_msg(clients[idx2].fd, "+Ok. Es tu turno");
    }
}

void end_game(int game_id, const char *reason) {
    Game *g = &games[game_id];
    if (!g->active) return;
    int t0 = count_territories(game_id, 0);
    int t1 = count_territories(game_id, 1);
    int u0 = total_units(game_id, 0);
    int u1 = total_units(game_id, 1);
    char winner[100];
    if (t0 > t1) strcpy(winner, "Jugador 1");
    else if (t1 > t0) strcpy(winner, "Jugador 2");
    else {
        if (u0 > u1) strcpy(winner, "Jugador 1");
        else if (u1 > u0) strcpy(winner, "Jugador 2");
        else strcpy(winner, "Empate");
    }
    for (int i = 0; i < 2; i++) {
        int cidx = g->clients[i];
        if (cidx >= 0 && clients[cidx].in_use) {
            send_msg(clients[cidx].fd, "+Ok. Fin de la partida. Gana %s. Territorios J1: %d, J2: %d.",
                     winner, t0, t1);
            clients[cidx].game_id = -1;
            clients[cidx].player_num = -1;
        }
    }
    g->active = 0;
    g->clients[0] = g->clients[1] = -1;
}

void next_turn(int game_id) {
    Game *g = &games[game_id];
    g->turn = 1 - g->turn;
    g->turn_count++;
    if (g->turn_count >= MAX_TURNS * 2) {
        end_game(game_id, "max turns");
        return;
    }
    if (count_territories(game_id, 0) == 0 || count_territories(game_id, 1) == 0) {
        end_game(game_id, "no territories");
        return;
    }
    int cur = g->clients[g->turn];
    int opp = g->clients[1 - g->turn];
    if (cur >= 0 && clients[cur].in_use) send_msg(clients[cur].fd, "+Ok. Es tu turno");
    if (opp >= 0 && clients[opp].in_use) send_msg(clients[opp].fd, "+Ok. Espera tu turno");
}

void handle_conquistar(int idx, char *orig, char *dest, int units) {
    Client *c = &clients[idx];
    if (c->game_id < 0) { send_msg(c->fd, "-Err. No estás en una partida"); return; }
    Game *g = &games[c->game_id];
    if (g->turn != c->player_num) { send_msg(c->fd, "-Err. No es tu turno"); return; }
    if (units <= 0) { send_msg(c->fd, "-Err. Unidades inválidas"); return; }
    int r1, c1, r2, c2;
    if (!parse_territory(orig, &r1, &c1) || !parse_territory(dest, &r2, &c2)) {
        send_msg(c->fd, "-Err. Territorio inválido"); return;
    }
    char pc = player_char(c->player_num);
    if (g->board[r1][c1] != pc) { send_msg(c->fd, "-Err. Territorio origen no es tuyo"); return; }
    if (g->units[r1][c1] < units) { send_msg(c->fd, "-Err. Unidades insuficientes"); return; }
    if (g->board[r2][c2] != 'N') { send_msg(c->fd, "-Err. Territorio destino no neutral"); return; }
    if (abs(r1 - r2) + abs(c1 - c2) != 1) { send_msg(c->fd, "-Err. Territorios no adyacentes"); return; }
    g->units[r1][c1] -= units;
    g->board[r2][c2] = pc;
    g->units[r2][c2] = units;
    send_msg(c->fd, "+Ok. Territorio %s conquistado.", dest);
    int opp = 1 - c->player_num;
    int opp_idx = g->clients[opp];
    if (opp_idx >= 0 && clients[opp_idx].in_use)
        send_msg(clients[opp_idx].fd, "+Ok. El oponente ha conquistado %s.", dest);
    next_turn(c->game_id);
}

void handle_atacar(int idx, char *orig, char *dest, int units) {
    Client *c = &clients[idx];
    if (c->game_id < 0) { send_msg(c->fd, "-Err. No estás en una partida"); return; }
    Game *g = &games[c->game_id];
    if (g->turn != c->player_num) { send_msg(c->fd, "-Err. No es tu turno"); return; }
    if (units <= 0) { send_msg(c->fd, "-Err. Unidades inválidas"); return; }
    int r1, c1, r2, c2;
    if (!parse_territory(orig, &r1, &c1) || !parse_territory(dest, &r2, &c2)) {
        send_msg(c->fd, "-Err. Territorio inválido"); return;
    }
    char pc = player_char(c->player_num);
    char oppc = player_char(1 - c->player_num);
    if (g->board[r1][c1] != pc) { send_msg(c->fd, "-Err. Territorio origen no es tuyo"); return; }
    if (g->board[r2][c2] != oppc) { send_msg(c->fd, "-Err. Territorio destino no es del oponente"); return; }
    if (g->units[r1][c1] < units) { send_msg(c->fd, "-Err. Unidades insuficientes"); return; }
    if (abs(r1 - r2) + abs(c1 - c2) != 1) { send_msg(c->fd, "-Err. Territorios no adyacentes"); return; }
    int opp = 1 - c->player_num;
    int opp_idx = g->clients[opp];
    if (units > g->units[r2][c2]) {
        g->units[r1][c1] -= units;
        g->board[r2][c2] = pc;
        g->units[r2][c2] = units;
        send_msg(c->fd, "+Ok. Has conquistado el territorio %s.", dest);
        if (opp_idx >= 0 && clients[opp_idx].in_use)
            send_msg(clients[opp_idx].fd, "+Ok. Has perdido el territorio %s.", dest);
    } else {
        g->units[r1][c1] -= units;
        send_msg(c->fd, "+Ok. Has perdido el ataque contra %s.", dest);
        if (opp_idx >= 0 && clients[opp_idx].in_use)
            send_msg(clients[opp_idx].fd, "+Ok. Has defendido el territorio %s.", dest);
    }
    next_turn(c->game_id);
}

void handle_reforzar(int idx, char *orig, char *dest, int units) {
    Client *c = &clients[idx];
    if (c->game_id < 0) { send_msg(c->fd, "-Err. No estás en una partida"); return; }
    Game *g = &games[c->game_id];
    if (g->turn != c->player_num) { send_msg(c->fd, "-Err. No es tu turno"); return; }
    if (units <= 0) { send_msg(c->fd, "-Err. Unidades inválidas"); return; }
    int r1, c1, r2, c2;
    if (!parse_territory(orig, &r1, &c1) || !parse_territory(dest, &r2, &c2)) {
        send_msg(c->fd, "-Err. Territorio inválido"); return;
    }
    char pc = player_char(c->player_num);
    if (g->board[r1][c1] != pc || g->board[r2][c2] != pc) {
        send_msg(c->fd, "-Err. Ambos territorios deben ser tuyos"); return;
    }
    if (g->units[r1][c1] < units) { send_msg(c->fd, "-Err. Unidades insuficientes"); return; }
    if (abs(r1 - r2) + abs(c1 - c2) != 1) { send_msg(c->fd, "-Err. Territorios no adyacentes"); return; }
    g->units[r1][c1] -= units;
    g->units[r2][c2] += units;
    send_msg(c->fd, "+Ok. Territorio reforzado.");
    next_turn(c->game_id);
}

void handle_pasar(int idx) {
    Client *c = &clients[idx];
    if (c->game_id < 0) { send_msg(c->fd, "-Err. No estás en una partida"); return; }
    Game *g = &games[c->game_id];
    if (g->turn != c->player_num) { send_msg(c->fd, "-Err. No es tu turno"); return; }
    send_msg(c->fd, "+Ok. Turno pasado.");
    next_turn(c->game_id);
}

void handle_iniciar(int idx) {
    Client *c = &clients[idx];
    if (c->game_id >= 0) { send_msg(c->fd, "-Err. Ya estás en una partida"); return; }
    if (waiting_client == idx) { send_msg(c->fd, "+Ok. Ya estás esperando"); return; }
    if (waiting_client == -1) {
        waiting_client = idx;
        send_msg(c->fd, "+Ok. Esperando otro jugador");
    } else {
        int active_games = 0;
        for (int i = 0; i < MAX_GAMES; i++) if (games[i].active) active_games++;
        if (active_games >= MAX_GAMES) {
            send_msg(c->fd, "-Err. No hay partidas disponibles");
            return;
        }
        int other = waiting_client;
        waiting_client = -1;
        start_game(other, idx);
    }
}

void disconnect_client(int idx) {
    Client *c = &clients[idx];
    if (!c->in_use) return;
    if (c->game_id >= 0) {
        Game *g = &games[c->game_id];
        int opp = 1 - c->player_num;
        int opp_idx = g->clients[opp];
        if (opp_idx >= 0 && clients[opp_idx].in_use) {
            send_msg(clients[opp_idx].fd, "+Ok. Tu oponente ha terminado la partida");
        }
        end_game(c->game_id, "disconnect");
    }
    if (waiting_client == idx) waiting_client = -1;
    close(c->fd);
    c->in_use = 0;
    c->authenticated = 0;
    c->game_id = -1;
    c->player_num = -1;
    c->inlen = 0;
}

void process_command(int idx, char *line) {
    Client *c = &clients[idx];
    if (!c->in_use) return;
    char cmd[64];
    if (sscanf(line, "%63s", cmd) != 1) return;

    if (strcmp(cmd, "USUARIO") == 0) {
        char user[50];
        if (sscanf(line, "USUARIO %49s", user) != 1) {
            send_msg(c->fd, "-Err. Formato USUARIO usuario");
            return;
        }
        if (c->authenticated) { send_msg(c->fd, "-Err. Ya validado"); return; }
        if (find_user(user) >= 0) {
            strcpy(c->pending_user, user);
            send_msg(c->fd, "+Ok. Usuario correcto");
        } else {
            send_msg(c->fd, "-Err. Usuario incorrecto");
        }
    } else if (strcmp(cmd, "PASSWORD") == 0) {
        char pass[50];
        if (sscanf(line, "PASSWORD %49s", pass) != 1) {
            send_msg(c->fd, "-Err. Formato PASSWORD contraseña");
            return;
        }
        if (c->authenticated) { send_msg(c->fd, "-Err. Ya validado"); return; }
        if (strlen(c->pending_user) == 0) {
            send_msg(c->fd, "-Err. Usuario no indicado");
            return;
        }
        int u = find_user(c->pending_user);
        if (u >= 0 && strcmp(users[u].pass, pass) == 0) {
            c->authenticated = 1;
            strcpy(c->user, c->pending_user);
            send_msg(c->fd, "+Ok. Usuario validado");
        } else {
            send_msg(c->fd, "-Err. Error en la validación");
        }
    } else if (strcmp(cmd, "REGISTRO") == 0) {
        char user[50], pass[50];
        if (sscanf(line, "REGISTRO %49s %49s", user, pass) != 2) {
            send_msg(c->fd, "-Err. Formato REGISTRO usuario password");
            return;
        }
        if (find_user(user) >= 0) {
            send_msg(c->fd, "-Err. Usuario ya existe");
        } else {
            add_user(user, pass);
            send_msg(c->fd, "+Ok. Usuario registrado");
        }
    } else if (strcmp(cmd, "SALIR") == 0) {
        send_msg(c->fd, "+Ok. Adios");
        disconnect_client(idx);
    } else if (!c->authenticated) {
        send_msg(c->fd, "-Err. Usuario no validado");
    } else if (strcmp(cmd, "INICIAR-PARTIDA") == 0) {
        handle_iniciar(idx);
    } else if (strcmp(cmd, "ESTADO") == 0) {
        send_state(idx);
    } else if (strcmp(cmd, "CONQUISTAR") == 0) {
        char orig[10], dest[10]; int units;
        if (sscanf(line, "CONQUISTAR %9s %9s %d", orig, dest, &units) != 3) {
            send_msg(c->fd, "-Err. Formato CONQUISTAR origen destino unidades");
            return;
        }
        handle_conquistar(idx, orig, dest, units);
    } else if (strcmp(cmd, "ATACAR") == 0) {
        char orig[10], dest[10]; int units;
        if (sscanf(line, "ATACAR %9s %9s %d", orig, dest, &units) != 3) {
            send_msg(c->fd, "-Err. Formato ATACAR origen destino unidades");
            return;
        }
        handle_atacar(idx, orig, dest, units);
    } else if (strcmp(cmd, "REFORZAR") == 0) {
        char orig[10], dest[10]; int units;
        if (sscanf(line, "REFORZAR %9s %9s %d", orig, dest, &units) != 3) {
            send_msg(c->fd, "-Err. Formato REFORZAR origen destino unidades");
            return;
        }
        handle_reforzar(idx, orig, dest, units);
    } else if (strcmp(cmd, "PASAR") == 0) {
        handle_pasar(idx);
    } else {
        send_msg(c->fd, "-Err. Comando no reconocido");
    }
}

void handle_read(int idx) {
    Client *c = &clients[idx];
    char buf[1024];
    int n = recv(c->fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        disconnect_client(idx);
        return;
    }
    buf[n] = 0;
    if (c->inlen + n >= (int)sizeof(c->inbuf)) {
        c->inlen = 0;
        send_msg(c->fd, "-Err. Línea demasiado larga");
        return;
    }
    memcpy(c->inbuf + c->inlen, buf, n);
    c->inlen += n;
    c->inbuf[c->inlen] = 0;

    char *start = c->inbuf;
    char *newline;
    while ((newline = strchr(start, '\n')) != NULL) {
        *newline = 0;
        int len = strlen(start);
        if (len > 0 && start[len - 1] == '\r') start[len - 1] = 0;
        process_command(idx, start);
        if (!c->in_use) return;
        start = newline + 1;
    }
    int remaining = c->inlen - (start - c->inbuf);
    if (remaining > 0) memmove(c->inbuf, start, remaining);
    c->inlen = remaining;
    c->inbuf[c->inlen] = 0;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    srand(time(NULL));
    add_user("j1", "j1");
    add_user("j2", "j2");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].in_use = 0;
        clients[i].fd = -1;
        clients[i].game_id = -1;
        clients[i].player_num = -1;
        clients[i].inlen = 0;
    }
    for (int i = 0; i < MAX_GAMES; i++) {
        games[i].active = 0;
        games[i].clients[0] = games[i].clients[1] = -1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 5) < 0) { perror("listen"); exit(1); }
    printf("Servidor escuchando en puerto %d\n", PORT);

    fd_set master, readfds;
    FD_ZERO(&master);
    FD_SET(server_fd, &master);
    int maxfd = server_fd;

    while (1) {
        readfds = master;
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select"); continue;
        }

        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int newfd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
            if (newfd < 0) { perror("accept"); }
            else {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].in_use) { slot = i; break; }
                }
                if (slot < 0) {
                    send_msg(newfd, "-Err. Servidor lleno");
                    close(newfd);
                } else {
                    clients[slot].fd = newfd;
                    clients[slot].in_use = 1;
                    clients[slot].authenticated = 0;
                    clients[slot].pending_user[0] = 0;
                    clients[slot].user[0] = 0;
                    clients[slot].game_id = -1;
                    clients[slot].player_num = -1;
                    clients[slot].inlen = 0;
                    FD_SET(newfd, &master);
                    if (newfd > maxfd) maxfd = newfd;
                    send_msg(newfd, "+Ok. Usuario conectado");
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && FD_ISSET(clients[i].fd, &readfds)) {
                handle_read(i);
                if (!clients[i].in_use) {
                    FD_CLR(clients[i].fd, &master);
                }
            }
        }
    }
    close(server_fd);
    return 0;
}
