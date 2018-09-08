#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAXNAME 80  /* maximum permitted name size, not including \0 */
#define NPITS 6  /* number of pits on a side, not including the end pit */
#define NPEBBLES 4 /* initial number of pebbles per pit */
#define MAXMESSAGE (MAXNAME + 50) /* maximum permitted message size */

#define WELCOME "Welcome to Mancala. What is your name?\r\n"
#define WELCOME_SIZE (strlen(WELCOME) + 1)
#define INVALID "Invalid username. DISCONNECTED. Try connect again.\r\n"
#define INVALID_SIZE (strlen(INVALID) + 1)
#define MOVE "Your move?\r\n"
#define MOVE_SIZE (strlen(MOVE) + 1)
#define NOT_MOVE "It is not your move.\r\n"
#define NOT_MOVE_SIZE (strlen(NOT_MOVE) + 1)
#define INVALID_PIT "Invalid pit index! Try again.\r\n"
#define INVALID_PIT_SIZE (strlen(INVALID_PIT) + 1)

#define REQUIRE_CONNECT "New player requires connection.\n"
#define INVALID_NAME_DISCONNECT "Disconnect a player due to invalid name.\n"

int port = 3000;
int listenfd;

struct player {
    int fd;
    char name[MAXNAME+1];
    int pits[NPITS+1];
    struct player *front;
    struct player *next;
    struct player *head;

    int wait_for_username;  /* set to 1 if accepted but waiting to check username, 0 if successfully add to game */
    char name_buf[MAXNAME+1]; /* store the name from the client, need to check the validity */
    int get_full_name;  /* set to 1 if get the full name, 0 otherwise */
    int inbuf;  /* number of bytes already read */

    int play;   /* set to 1 if it is this player's turn to play, 0 otherwise */
    int disconnect; /* set to 1 if disconnect to the server, 0 if connect to the server */
};
struct player *playerlist = NULL;


extern void parseargs(int argc, char **argv);
extern void makelistener();
extern int compute_average_pebbles();
extern int game_is_over();
extern void broadcast(char *s, struct player *not_announce);

int accept_connection(int listenfd);
void reset_pits(struct player *current_player, int pebbles);
void initialize_player(int client_fd);
int find_newline(const char *buf, int n);
char *read_from(int fd);
void display_game_state();
char *disconnect(int disconnect_fd);
void disconnect_invalid_name(int fd, fd_set all_fds);
void turn_game(struct player *turn_player, int pit_index);
int get_number_players();
struct player *get_player(int fd);
struct player *get_current_player();
struct player *get_next_player(struct player *current_player);


int main(int argc, char **argv) {
    char msg[MAXMESSAGE];

    parseargs(argc, argv);
    makelistener();

    int max_fd = listenfd;
    fd_set all_fds;
    FD_ZERO(&all_fds);
    FD_SET(listenfd, &all_fds);

    while (!game_is_over()) {
        fd_set listen_fds = all_fds;

        int nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
        if (nready == -1) {
            perror("server: select");
            exit(1);
        }

        // new player requires connection
        if (FD_ISSET(listenfd, &listen_fds)) {
            int client_fd = accept_connection(listenfd);
            if (client_fd > max_fd) {
                max_fd = client_fd;
            }
            FD_SET(client_fd, &all_fds);
        }

        char announce_new_player[MAXMESSAGE];
        // for loop playerlist, check which players (or potential players) are active
        for (struct player *p = playerlist; p; p = p->next) {
            if (FD_ISSET(p->fd, &listen_fds)) { // current player is active
                if (p->wait_for_username == 1) {    // wait to check valid username
                    char buf[MAXNAME + 1] = {'\0'};
                    int nbytes;

                    nbytes = read(p->fd, &buf, (MAXNAME + 1 - p->inbuf));
                    if (nbytes == 0) {
                        disconnect_invalid_name(p->fd, all_fds);
                    }
                    p->inbuf += nbytes;

                    if (p->inbuf < (MAXNAME + 1)) {
                        strncat(p->name_buf, buf, strlen(buf) + 1);
                    } else {    // invalid case1: the username is too long
                        FD_CLR(p->fd, &all_fds);
                        disconnect(p->fd);
                        printf("%s", INVALID_NAME_DISCONNECT);
                    }

                    int newline = find_newline(p->name_buf, strlen(p->name_buf));
                    if (newline != -1) {
                        if (newline == 0) { // invalid case2: enter return immediately
                            if (write(p->fd, INVALID, INVALID_SIZE) != INVALID_SIZE) {
                                perror("Server: write");
                                exit(1);
                            }
                            disconnect_invalid_name(p->fd, all_fds);
                        } else {
                            p->get_full_name = 1;
                            p->name_buf[find_newline(p->name_buf, strlen(p->name_buf))] = '\0';
                        }
                    }

                    if (p->get_full_name == 1) {
                        // invalid case3: username already exists
                        for (struct player* g = playerlist; g; g = g->next) {
                            if ((g->wait_for_username == 0) &&
                                strncmp(g->name, p->name_buf, strlen(p->name_buf) + 1) == 0) {
                                if (write(p->fd, INVALID, INVALID_SIZE) != INVALID_SIZE) {
                                    perror("Server: write");
                                    exit(1);
                                }
                                disconnect_invalid_name(p->fd, all_fds);
                            }
                        }

                        // Get valid name
                        strncpy(p->name, p->name_buf, MAXNAME + 1);
                        p->wait_for_username = 0;
                    }

                    if (p->wait_for_username == 0 && p->disconnect == 0)  {   // get valid name, add to the game
                        sprintf(announce_new_player, "Player %s is joining in.\r\n", p->name);
                        broadcast(announce_new_player, NULL);
                        printf("Player %s is joining in.\n", p->name);

                        // if this is the first player, begin the game immediately
                        if (get_number_players() == 1) {
                            p->play = 1;
                        }

                        // announce game state and prompt message to get next active fd
                        display_game_state();
                        struct player *current_player = get_current_player();
                        if (current_player != NULL) {
                            if (write(current_player->fd, MOVE, MOVE_SIZE) != MOVE_SIZE) {
                                perror("Server: write");
                                exit(1);
                            }
                            char announce[MAXMESSAGE + 1];
                            sprintf(announce, "It is %s's move\r\n", current_player->name);
                            broadcast(announce, current_player);
                            printf("It is %s's move.\n", current_player->name);
                        }
                    }
                } else {    // current player is already in the game
                    if (p->play == 1) { // it is current player's turn to play
                        // Call read_from which contains while loop, may potentially block other players.
                        // By the handout, we only need to consider this situation in the read full name part
                        char *read_number = read_from(p->fd);

                        if (p->disconnect == 1) {    // current player disconnects when it is his turn to play
                            get_next_player(p)->play = 1;

                            FD_CLR(p->fd, &all_fds);
                            char *disconnect_name = disconnect(p->fd);
                            char announce_disconnect[MAXMESSAGE + 1];
                            sprintf(announce_disconnect, "Player %s disconnected.\r\n", disconnect_name);
                            broadcast(announce_disconnect, p);
                            printf("Player %s disconnected.\n", disconnect_name);

                            // announce new player in the game that it is his turn
                            struct player *current_player = get_current_player();
                            if (current_player != NULL) {
                                if (write(current_player->fd, MOVE, MOVE_SIZE) != MOVE_SIZE) {
                                    perror("Server: write");
                                    exit(1);
                                }
                                char announce[MAXMESSAGE + 1];
                                sprintf(announce, "It is %s's move\r\n", current_player->name);
                                broadcast(announce, get_current_player());
                                printf("It is %s's move.\n", current_player->name);
                            }
                        } else {    // current player in the game, check whether the index is valid
                            if (strlen(read_number) == 0) { // enter nothing, return immediately
                                if (write(p->fd, INVALID_PIT, INVALID_PIT_SIZE) != INVALID_PIT_SIZE) {
                                    perror("Server: write");
                                    exit(1);
                                }
                            } else {    // enter a number
                                int potential_index = strtol(read_number, NULL, 0);

                                if (potential_index < 0 ||
                                    potential_index > (NPITS - 1)) {    // case1: pit index out of range
                                    if (write(p->fd, INVALID_PIT, INVALID_PIT_SIZE) != INVALID_PIT_SIZE) {
                                        perror("Server: write");
                                        exit(1);
                                    }
                                } else {    // case2: pit index within range but with no pebble
                                    if (p->pits[potential_index] == 0) {
                                        if (write(p->fd, INVALID_PIT, INVALID_PIT_SIZE) != INVALID_PIT_SIZE) {
                                            perror("Server: write");
                                            exit(1);
                                        }
                                    } else {    // case3: it is the valid index
                                        char announcement[MAXMESSAGE + 1];
                                        sprintf(announcement, "Player %s distributes %d pebble(s) in pit index %d.\n\r",
                                                p->name, p->pits[potential_index], potential_index);
                                        broadcast(announcement, NULL);
                                        printf("Player %s distributes %d pebble(s) in pit index %d.\n",
                                               p->name, p->pits[potential_index], potential_index);

                                        // play the game
                                        turn_game(p, potential_index);

                                        // announce game state and prompt message to get next active fd
                                        display_game_state();
                                        struct player *current_player = get_current_player();
                                        if (current_player != NULL) {
                                            if (write(current_player->fd, MOVE, MOVE_SIZE) != MOVE_SIZE) {
                                                perror("Server: write");
                                                exit(1);
                                            }
                                            char announce[MAXMESSAGE + 1];
                                            sprintf(announce, "It is %s's move\r\n", current_player->name);
                                            broadcast(announce, get_current_player());
                                            printf("It is %s's move.\n", current_player->name);
                                        }
                                    }
                                }
                            }
                        }
                    } else {    // it is not current player's turn to play
                        // Call read_from which contains while loop, may potentially block other players.
                        // By the handout, we only need to consider this situation in the read full name part
                        read_from(p->fd);   // Read out junk message.

                        if (write(p->fd, NOT_MOVE, NOT_MOVE_SIZE) != NOT_MOVE_SIZE) {
                            perror("Server: write");
                            exit(1);
                        }

                        // Check disconnection
                        if (p->disconnect == 1) {
                            FD_CLR(p->fd, &all_fds);
                            char *disconnect_name = disconnect(p->fd);
                            char announce_disconnect[MAXMESSAGE + 1];
                            sprintf(announce_disconnect, "Player %s disconnected.\r\n", disconnect_name);
                            broadcast(announce_disconnect, p);
                            printf("Player %s disconnected.\n", disconnect_name);
                        }
                    }
                }
            }
        }
    }
    broadcast("Game over!\r\n", NULL);
    printf("Game over!\n");
    for (struct player *p = playerlist; p; p = p->next) {
        int points = 0;
        for (int i = 0; i <= NPITS; i++) {
            points += p->pits[i];
        }
        printf("%s has %d points\n", p->name, points);
        snprintf(msg, MAXMESSAGE, "%s has %d points\r\n", p->name, points);
        broadcast(msg, NULL);
    }

    return 0;
}

void parseargs(int argc, char **argv) {
    int c, status = 0;
    while ((c = getopt(argc, argv, "p:")) != EOF) {
        switch (c) {
            case 'p':
                port = strtol(optarg, NULL, 0);
                break;
            default:
                status++;
        }
    }
    if (status || optind != argc) {
        fprintf(stderr, "usage: %s [-p port]\n", argv[0]);
        exit(1);
    }
}

void makelistener() {
    struct sockaddr_in r;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *) &on, sizeof(on)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&r, sizeof(r))) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
}

/* call this BEFORE linking the new player in to the list */
int compute_average_pebbles() {
    struct player *p;
    int i;

    if (playerlist == NULL) {
        return NPEBBLES;
    }

    int nplayers = 0, npebbles = 0;
    for (p = playerlist; p; p = p->next) {
        nplayers++;
        for (i = 0; i < NPITS; i++) {
            npebbles += p->pits[i];
        }
    }
    return ((npebbles - 1) / nplayers / NPITS + 1);  /* round up */
}

int game_is_over() { /* boolean */
    int i;

    if (!playerlist) {
        return 0;  /* we haven't even started yet! */
    }

    for (struct player *p = playerlist; p; p = p->next) {
        int is_all_empty = 1;
        for (i = 0; i < NPITS; i++) {
            if (p->pits[i]) {
                is_all_empty = 0;
            }
        }
        if (is_all_empty) {
            return 1;
        }
    }
    return 0;
}

/*
 * Broadcast the message s to all the players excpets the player not_announce.
 */
void broadcast(char *s, struct player *not_announce) {
    for (struct player* p = playerlist; p; p = p->next) {
        if (p != not_announce && p->wait_for_username == 0) {
            if (write(p->fd, s, strlen(s) + 1) != (strlen(s) + 1)) {
                perror("Server: broadcast");
                exit(1);
            }
        }
    }
}

/*
 * Accept the connection request from listenfd.
 * Return 0 if successfully connected,
 * otherwise there is a disconnection and return clientfd.
 */
int accept_connection(int listenfd) { // the file descriptor used for listen
    int client_fd = accept(listenfd, NULL, NULL);
    printf(REQUIRE_CONNECT);
    if (client_fd < 0) {
        perror("server: accept");
        close(listenfd);
        exit(1);
    }
    if (write(client_fd, WELCOME, WELCOME_SIZE) != WELCOME_SIZE) {
        perror("server: write");
        exit(1);
    }
    initialize_player(client_fd);
    return client_fd;
}

/*
 * Reset the numbers of peddles in each pits of current player.
 */
void reset_pits(struct player *current_player, int pebbles) {
    for (int i = 0; i < NPITS; i++) {
        current_player->pits[i] = pebbles;
    }
}

/*
 * Initialize the player struct with given client_fd, add to linked list.
 */
void initialize_player(int client_fd) {
    struct player *new_player = malloc(sizeof(struct player));

    new_player->fd = client_fd;

    if (playerlist == NULL) { // it is the first player
        reset_pits(new_player, NPEBBLES);   // avoid setting end pits
        new_player->next = playerlist;
        playerlist = new_player;
        new_player->front = NULL;
    } else {
        int average_pebbles = compute_average_pebbles();
        reset_pits(new_player, average_pebbles);

        new_player->next = playerlist;
        playerlist = new_player;
        new_player->next->front = new_player;
        new_player->front = NULL;
    }
    new_player->pits[NPITS] = 0;

    new_player->wait_for_username = 1;
    new_player->play = 0;
    new_player->disconnect = 0;
    new_player->get_full_name = 0;
    new_player->inbuf = 0;

    for (struct player *p = playerlist; p; p = p->next) {
        p->head = new_player;
    }
}


/*
 * Return the index of first occurrence of \n,
 * otherwise return -1.
 */
int find_newline(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\r') {
            return i;
        } else if (buf[i] == '\n') {
            return i;
        }
    }
    return -1;
}

/*
 * Return the whole line at once.
 */
char *read_from(int fd) {
    char buf[MAXMESSAGE + 1] = {'\0'};
    int inbuf = 0;           // number bytes currently in buf
    int room = MAXMESSAGE + 1;  // number bytes remaining in buf
    int newline_flag = -1;
    int nbytes;
    char *whole_line = malloc(sizeof(char) * (MAXMESSAGE + 1));

    // Continue reading until gets newline character
    while (newline_flag < 0 && (nbytes = read(fd, &buf, room)) > 0) {
        if (nbytes == 0) {
            get_player(fd)->disconnect = 1;
        }

        inbuf += nbytes;
        if ((newline_flag = find_newline(buf, inbuf)) < 0) {
            room -= nbytes;
        }
        strncat(whole_line, buf, room);
        room -= nbytes;
    }
    if (nbytes == 0) {
        get_player(fd)->disconnect = 1;
    }

    whole_line[find_newline(whole_line, strlen(whole_line))] = '\0';
    return whole_line;
}

/*
 * Disconnect the passed in fd in the fd set all_fds.
 */
void disconnect_invalid_name(int fd, fd_set all_fds) {
    FD_CLR(fd, &all_fds);
    disconnect(fd);
    printf("%s", INVALID_NAME_DISCONNECT);
}

/*
 * Display the game state to all the players and the server.
 */
void display_game_state() {
    int num_players = get_number_players();
    char *game_state = malloc(sizeof(char) * (MAXNAME + 2 + MAXMESSAGE + 3) * num_players);

    for (struct player* p = playerlist; p; p = p->next) {
        if (p->wait_for_username == 0) {
            char line[MAXNAME + 2 + MAXMESSAGE + 3];
            sprintf(line, "%s:", p->name);

            for (int i = 0; i < NPITS; i++) {
                char pit_state[MAXMESSAGE] = {'\0'};
                sprintf(pit_state, " [%d]%d", i, p->pits[i]);
                strncat(line, pit_state, strlen(pit_state) + 1);
            }
            char pit_state[MAXMESSAGE] = {'\0'};
            sprintf(pit_state, " [end pit]%d\r\n", p->pits[NPITS]);
            strncat(line, pit_state, strlen(pit_state) + 1);
            strncat(game_state, line, strlen(line) + 1);
        }
    }
    broadcast(game_state, NULL);
    printf("%s", game_state);
}

/*
 * Remove the player with disconnect_fd from the playerlist.
 * Return the name of the disconnected player if avaliable.
 */
char *disconnect(int disconnect_fd) {
    struct player *disconnect_player = get_player(disconnect_fd);
    char *disconnect_name = malloc(sizeof(char) * (MAXNAME + 1));
    disconnect_player->disconnect = 1;

    if (disconnect_player->wait_for_username == 0) {
        strncpy(disconnect_name, disconnect_player->name, MAXNAME + 1);
    } else {
        strncpy(disconnect_name, INVALID_NAME_DISCONNECT, MAXNAME + 1);
    }

    // disconnect when it is his turn play
    if (disconnect_player->play == 1) {
        get_next_player(disconnect_player)->play = 1;
    }

    if (get_number_players() == 1
        && get_current_player() == disconnect_player) {   // disconnects only player in the game
        playerlist = NULL;
    } else if (disconnect_player->next == NULL
               && disconnect_player->front == NULL) {   // disconnect only player not in the game
        playerlist = NULL;
    } else if (disconnect_player->head == disconnect_player) {  // disconnect the head
        struct player *new_head = disconnect_player->next;
        new_head->front = NULL;
        playerlist = new_head;
        for (struct player *p = playerlist; p; p = p->next) {
            p->head = new_head;
        }
    } else if (disconnect_player->next == NULL) {   // disconnect the tail
        disconnect_player->front->next = NULL;
    } else {    // disconnect middle player
        struct player *next = disconnect_player->next;
        struct player *front = disconnect_player->front;
        front->next = next;
        next->front = front;
    }

    return disconnect_name;
}

/*
 * Play game in one turn.
 */
void turn_game(struct player *turn_player, int pit_index) {
    struct player *current_distribute = turn_player;
    int distribute_pit_index = pit_index + 1;
    int pebbles = turn_player->pits[pit_index]; // number of pits to distribute
    int play_again = 0; // set to 0 if current player can play again

    turn_player->pits[pit_index] = 0;

    while (pebbles > 0) {
        // distribute the pebbles on the side of player himself
        if (current_distribute == turn_player) {
            while (distribute_pit_index < (NPITS + 1) && pebbles > 0) {
                current_distribute->pits[distribute_pit_index] += 1;
                pebbles -= 1;
                distribute_pit_index += 1;
            }

            // check whether play again
            if (distribute_pit_index == (NPITS + 1) && pebbles == 0) {  // play again
                play_again = 1;
            }
        } else {    // distribute the pebbles on the side of other players
            while (distribute_pit_index < NPITS && pebbles > 0) {
                current_distribute->pits[distribute_pit_index] += 1;
                pebbles -= 1;
                distribute_pit_index += 1;
            }
        }

        if (pebbles > 0) {
            if (current_distribute->next == NULL) {
                current_distribute = current_distribute->head;
            } else {
                current_distribute = current_distribute->next;
            }
            distribute_pit_index = 0;
        }
    }

    if (play_again == 0) {
        turn_player->play = 0;
        get_next_player(turn_player)->play = 1;
    }
}


/*
 * Get number of player in the game pool.
 */
int get_number_players() {
    int players = 0;
    for (struct player *p = playerlist; p; p = p->next) {
        if (p->wait_for_username == 0) {
            players++;
        }
    }
    return players;
}

/*
 * Get the player with input fd.
 */
struct player*get_player(int fd) {
    struct player *player = NULL;
    for (struct player *p = playerlist; p; p = p->next) {
        if (p->fd == fd) {
            player = p;
        }
    }
    return player;
}

/*
 * Return the next player of current_player in the game.
 */
struct player *get_next_player(struct player *current_player) {
    struct player *next_player = NULL;
    if (current_player->next != NULL) {
        next_player = current_player->next;
    } else {
        next_player = current_player->head;
        while (next_player->wait_for_username != 0) {
            next_player = next_player->next;
        }
    }
    return next_player;
}

/*
 * Return the player who plays the current turn.
 */
struct player *get_current_player() {
    for (struct player *p = playerlist; p; p = p->next) {
        if (p->play == 1) {
            return p;
        }
    }
    return NULL;
}


