#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <algorithm>
#include <assert.h>

#include "siktacka-input-server.h"
#include "siktacka-establish-server.h"
#include "siktacka-communication-server.h"
#include "siktacka-game-logic.h"

// TODO final file should be siktacka_server

// NOTE: pass broadcaster

// broadcast at hoc

// TODO send after request

// TODO set broadcaster

struct player {
    bool connected;
    bool ready;
    sockaddr_in6 address;
    uint64_t session_id;
    std::string name;
    uint64_t last_sent;
    int8_t direction;
    uint32_t expected_no;
    uint8_t id;

    player() {}

    player(bool connected, sockaddr_in6 address, uint64_t session_id,
                std::string name, uint64_t last_sent, int8_t direction,
                uint32_t expected_no) :
                connected(connected),
                address(address),
                session_id(session_id),
                name(name),
                last_sent(last_sent),
                direction(direction),
                expected_no(expected_no),
                id(MAX_PLAYERS) {

        if (direction != 0)
            ready = true;
        else
            ready = false;
    }
};

struct stats {
    int ready;
    int connected;
    int active;
    int inactive;
};



void send_client(pollfd &sock, uint32_t event_no, std::vector<event> &events,
            sockaddr_in6 &client_address, uint32_t game_id,
            std::vector<uint8_t> &stringifed, std::vector<std::string> &event_string) {

    uint32_t i = event_no;
    message_stc msg;
    std::string to_send;

    while (i < events.size()) {

        to_send = "";

        append_data<uint32_t>(game_id, to_send);

        for (; i < events.size(); i++) {

            if (stringifed[i] == 0) {

                event_string[i] = event_str(events[i]);;
                stringifed[i] = 1;
            }

            if (to_send.size() + event_string[i].size() > MAX_UDP_DATA)
                break;

            to_send += event_string[i];
        }

        send_string(sock, to_send, client_address);
    }
}


void broadcast(uint32_t game_id, std::vector<event> &events,
            std::vector<player> players, pollfd &sock) {

    std::vector<uint8_t> stringifed(events.size());

    std::vector<std::string> event_string(events.size());

    for (player p : players) {
        send_client(sock, p.expected_no, events, p.address, game_id,
            stringifed, event_string);
    }
}


bool same_addr(sockaddr_in6 a1, sockaddr_in6 a2) {
	return (a1.sin6_addr.s6_addr == a2.sin6_addr.s6_addr
                && a1.sin6_port == a2.sin6_port);
}


int check_sock(pollfd &sock, bool in_game) {

    int ret;
    int mode = (in_game ? 0 : -1);

    sock.revents = 0;

    ret = poll(&sock, 1, mode);

    if (ret <= 0) {
        std::cout << "Unexpected poll error\n";
        return -1;
    }

    if (!(sock.revents && POLLIN)) {
        return 0;
    }

    return 1;
}


int process_msg_cts(pollfd &sock, sockaddr_in6 &client_address,
                message_cts &msg_cts) {

    int len;
    int flags;
    socklen_t rcva_len;
    char client_buf[MAX_CS_DATAGRAM];
    std::string client_str;

    flags = 0;
    rcva_len = (socklen_t) sizeof(client_address);

    len = recvfrom(sock.fd, client_buf, sizeof(client_buf), flags,
                (struct sockaddr *) &client_address, &rcva_len);

    client_str = std::string(client_buf, len);

    return message_cts_from_str(client_str, msg_cts);
}


int check_existing(std::vector<player> &players, stats &sta,
            message_cts &msg_cts, sockaddr_in6 &client_address) {

    int pos = -1;

    for (uint32_t i = 0; i < MAX_PLAYERS; i++) {

        if (same_addr(client_address, players[i].address)) {
            pos = i;

            // NOTE: can player_name change?
            if (players[i].session_id < msg_cts.session_id
                    || players[i].name != msg_cts.player_name) {

                players[i].connected = false;
                return -2;
            }

            players[i].direction = msg_cts.turn_direction;
            players[i].session_id = msg_cts.session_id;
            players[i].expected_no = msg_cts.next_expected_event_no;
            players[i].last_sent = current_us();

            if (msg_cts.turn_direction != 0 && players[i].ready == 0) {

                players[i].ready = true;
                sta.ready ++;
            }

            break;
        }
    }

    return pos;
}

int make_place(std::vector<player> &players, stats &sta, message_cts &msg_cts) {

    for (uint32_t i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].connected
                && current_us() - players[i].last_sent > 2000000) {

            players[i].connected = false;

            sta.connected --;

            if (players[i].name != "") {
                sta.active --;

                if (players[i].ready)
                    sta.ready --;

            } else {

                sta.inactive --;
            }
        }

        if (players[i].connected && players[i].name == msg_cts.player_name) {
            return -2;
        }
    }

    return -1;
}


int seek_place(std::vector<player> &players, stats &sta,
            message_cts &msg_cts, sockaddr_in6 &client_address) {

    for (uint32_t i = 0; i < MAX_PLAYERS; i++) {

        if (!players[i].connected) {

            players[i] = player(true, client_address, msg_cts.session_id,
                        msg_cts.player_name, current_us(), msg_cts.turn_direction,
                        msg_cts.next_expected_event_no);

            sta.connected ++;

            if (msg_cts.player_name != "") {
                sta.active ++;

            } else {
                sta.inactive ++;
            }

            if (msg_cts.turn_direction != 0) {

                players[i].ready = true;
                sta.ready ++;
            }

            return 1;
        }
    }

    return MAX_PLAYERS;
}


int receive_msg_cts(bool in_game, pollfd &sock, std::vector<player> &players,
            stats &sta, std::vector<event> &events, uint32_t game_id = 0) {

    message_cts msg_cts;
    sockaddr_in6 client_address;

    int checked_sock = check_sock(sock, in_game);

    if (checked_sock != 1)
        return checked_sock;

    if (!process_msg_cts(sock, client_address, msg_cts))
        return 0;

    int pos = check_existing(players, sta, msg_cts, client_address);

    if (pos == -1) {
        pos = make_place(players, sta, msg_cts);
    }

    if (pos == -1) {
        pos = seek_place(players, sta, msg_cts, client_address);

    } else if (pos == -2) {
        return -1;
    }

    if ((uint32_t) pos < MAX_PLAYERS) {
        if (in_game) {
            std::vector<uint8_t> stringifed(events.size());
            std::vector<std::string> event_string(events.size());

            send_client(sock, msg_cts.next_expected_event_no, events,
                    client_address, game_id, stringifed, event_string);
        }
        return 1;
    }

    else
        return 0;
}


void make_players_list(std::vector<std::string> &player_names,
            std::vector<player> players) {

    uint32_t datagram_size = 4;

    player_names = std::vector<std::string>();

    for (uint32_t i = 0; i < MAX_PLAYERS; i++) {

        if (players[i].connected && players[i].name != "") {
            datagram_size += players[i].name.size();

            if (datagram_size > MAX_UDP_DATA)
                break;

            player_names.push_back(players[i].name);
        }
    }

    std::sort(player_names.begin(), player_names.end());

    for (uint32_t i = 0; i < player_names.size(); i++) {
        for (uint32_t j = 0; j < MAX_PLAYERS; j++) {

            if (players[j].name == player_names[i])
                players[j].id = i;
        }
    }
}


void prepare_dir_table(std::vector<int8_t> &dir_table,
            std::vector<player> players) {

    dir_table = std::vector<int8_t>(MAX_PLAYERS);

    for (player p : players) {
        if (p.id < MAX_PLAYERS) {
            dir_table[p.id] = p.direction;
        }
    }
}


int main(int argc, char *argv[]) {

    //socklen_t snda_len = (socklen_t) sizeof(client_address);
    int res;

    server_params sp;

    sockaddr_in6 address;

    pollfd sock;

    uint64_t last_round;

    uint64_t round_time;

    std::vector<player> players (MAX_PLAYERS);

    std::vector<int8_t> dir_table(MAX_PLAYERS);

    std::vector<std::string> player_names;

    stats sta;

    bool in_game;


    if (!fill_server_params(sp, argc, argv))
        return 1;
    print_server_params(sp);

    if (!establish_address(address, sp.port))
        return 1;

    if (!get_socket(sock))
        return 1;

    if (!bind_socket(sock, address))
        return 1;


    round_time = 1000000 / sp.speed;

    in_game = false;

    std::vector<event> empty_events;

    while(true) {

        res = receive_msg_cts(in_game, sock, players, sta, empty_events);

        if (res == -1)
            return -1;

        if (res == 0)
            break;


        if (sta.active >= 2 && sta.ready + sta.inactive == sta.connected) {

            in_game = true;

            make_players_list(player_names, players);

            game_state gs = new_game(player_names, sp);

            broadcast(gs.game_id, gs.all_events, players, sock);

            last_round = current_us();

            while (true) {

                while (current_us() - last_round > round_time) {
                    sock.revents = 0;

                    res = receive_msg_cts(in_game, sock, players, sta,
                            gs.all_events, gs.game_id);

                    if (res == -1)
                        return -1;
                }

                last_round += round_time;

                prepare_dir_table(dir_table, players);

                round(gs, sp, dir_table);

                broadcast(gs.game_id, gs.all_events, players, sock);

                if (gs.all_events.back().event_type == GAME_OVER)
                    break;
            }

            for (uint32_t i = 0; i < MAX_PLAYERS; i++) {
                players[i].ready == false;
                players[i].id = MAX_PLAYERS;
            }

            sta.ready = 0;

            in_game = false;
        }
    }

    return 0;
}
