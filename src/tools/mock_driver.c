#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

struct driver_state {
    char socket_path[PATH_MAX];
    int listen_fd;
    int client_fd;
    char node_id[ANNEAU_MAX_NODE_ID];
    char host[ANNEAU_MAX_HOST];
    uint16_t listen_port;
};

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signum)
{
    (void)signum;
    keep_running = 0;
}

static int create_server_socket(const char *socket_path)
{
    int fd = -1;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    unlink(socket_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    anneau_copy_field(addr.sun_path, sizeof(addr.sun_path), socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static int send_status(int fd, uint16_t type, int32_t code, uint32_t request_id, const char *message)
{
    struct anneau_status_payload payload;

    memset(&payload, 0, sizeof(payload));
    payload.code = code;
    anneau_copy_field(payload.message, sizeof(payload.message), message);
    return anneau_send_frame(fd, type, request_id, &payload, (uint32_t)sizeof(payload));
}

static int send_text_event(int fd, const char *source, const char *destination, const char *text)
{
    struct anneau_text_event header;
    size_t text_len = strlen(text);
    size_t payload_len = sizeof(header) + text_len;
    uint8_t *payload = malloc(payload_len);
    int rc = -1;

    if (payload == NULL) {
        perror("malloc");
        return -1;
    }

    memset(&header, 0, sizeof(header));
    anneau_copy_field(header.source, sizeof(header.source), source);
    anneau_copy_field(header.destination, sizeof(header.destination), destination);
    header.text_len = (uint32_t)text_len;
    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), text, text_len);

    rc = anneau_send_frame(fd, ANNEAU_MSG_TEXT_EVT, 0, payload, (uint32_t)payload_len);
    free(payload);
    return rc;
}

static int send_broadcast_event(int fd, const char *source, const char *text)
{
    struct anneau_broadcast_event header;
    size_t text_len = strlen(text);
    size_t payload_len = sizeof(header) + text_len;
    uint8_t *payload = malloc(payload_len);
    int rc = -1;

    if (payload == NULL) {
        perror("malloc");
        return -1;
    }

    memset(&header, 0, sizeof(header));
    anneau_copy_field(header.source, sizeof(header.source), source);
    header.text_len = (uint32_t)text_len;
    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), text, text_len);

    rc = anneau_send_frame(fd, ANNEAU_MSG_BROADCAST_EVT, 0, payload, (uint32_t)payload_len);
    free(payload);
    return rc;
}

static int send_topology_response(const struct driver_state *state)
{
    struct anneau_topology_response response;
    struct anneau_peer_info peers[2];
    uint8_t payload[sizeof(response) + sizeof(peers)];

    memset(&response, 0, sizeof(response));
    response.count = 2;
    memset(peers, 0, sizeof(peers));

    anneau_copy_field(peers[0].node_id, sizeof(peers[0].node_id),
                      state->node_id[0] != '\0' ? state->node_id : "node-local");
    anneau_copy_field(peers[0].host, sizeof(peers[0].host),
                      state->host[0] != '\0' ? state->host : "127.0.0.1");
    peers[0].input_port = state->listen_port != 0 ? state->listen_port : 9000;
    peers[0].output_port = state->listen_port != 0 ? (uint16_t)(state->listen_port + 1) : 9001;
    peers[0].state = 1;

    anneau_copy_field(peers[1].node_id, sizeof(peers[1].node_id), "node-voisin");
    anneau_copy_field(peers[1].host, sizeof(peers[1].host), "127.0.0.1");
    peers[1].input_port = 9100;
    peers[1].output_port = 9101;
    peers[1].state = 1;

    memcpy(payload, &response, sizeof(response));
    memcpy(payload + sizeof(response), peers, sizeof(peers));

    return anneau_send_frame(state->client_fd, ANNEAU_MSG_TOPOLOGY_RSP, 0,
                             payload, (uint32_t)sizeof(payload));
}

static int echo_file(struct driver_state *state, const struct anneau_frame *frame)
{
    return anneau_send_frame(state->client_fd, (uint16_t)(frame->header.type + 19), 0,
                             frame->payload, frame->header.payload_len);
}

static int handle_frame(struct driver_state *state, const struct anneau_frame *frame)
{
    switch (frame->header.type) {
    case ANNEAU_MSG_HELLO:
        return send_status(state->client_fd, ANNEAU_MSG_ACK, 0, frame->header.request_id,
                           "Driver mock prêt");
    case ANNEAU_MSG_JOIN_REQ: {
        const struct anneau_join_request *join = (const struct anneau_join_request *)frame->payload;
        if (frame->header.payload_len < sizeof(*join)) {
            return send_status(state->client_fd, ANNEAU_MSG_ERROR, -1, frame->header.request_id,
                               "JOIN invalide");
        }
        anneau_copy_field(state->node_id, sizeof(state->node_id), join->node_id);
        anneau_copy_field(state->host, sizeof(state->host), join->listen_host);
        state->listen_port = join->listen_port;
        return send_status(state->client_fd, ANNEAU_MSG_STATUS_EVT, 0, frame->header.request_id,
                           join->flags == 1 ? "Anneau créé (mock)" : "Joint à l'anneau (mock)");
    }
    case ANNEAU_MSG_LEAVE_REQ:
        return send_status(state->client_fd, ANNEAU_MSG_STATUS_EVT, 0, frame->header.request_id,
                           "Déconnexion acceptée (mock)");
    case ANNEAU_MSG_SEND_TEXT_REQ: {
        const struct anneau_text_request *header = (const struct anneau_text_request *)frame->payload;
        const char *text = NULL;
        if (frame->header.payload_len < sizeof(*header) ||
            frame->header.payload_len < sizeof(*header) + header->text_len) {
            return send_status(state->client_fd, ANNEAU_MSG_ERROR, -1, frame->header.request_id,
                               "Message texte invalide");
        }
        text = (const char *)(frame->payload + sizeof(*header));
        if (send_status(state->client_fd, ANNEAU_MSG_ACK, 0, frame->header.request_id,
                        "Message unicast pris en charge (mock)") < 0) {
            return -1;
        }
        return send_text_event(state->client_fd, state->node_id[0] ? state->node_id : "remote-node",
                               header->destination, text);
    }
    case ANNEAU_MSG_BROADCAST_REQ: {
        const struct anneau_broadcast_request *header =
            (const struct anneau_broadcast_request *)frame->payload;
        const char *text = NULL;
        if (frame->header.payload_len < sizeof(*header) ||
            frame->header.payload_len < sizeof(*header) + header->text_len) {
            return send_status(state->client_fd, ANNEAU_MSG_ERROR, -1, frame->header.request_id,
                               "Diffusion invalide");
        }
        text = (const char *)(frame->payload + sizeof(*header));
        if (send_status(state->client_fd, ANNEAU_MSG_ACK, 0, frame->header.request_id,
                        "Diffusion prise en charge (mock)") < 0) {
            return -1;
        }
        return send_broadcast_event(state->client_fd,
                                    state->node_id[0] ? state->node_id : "remote-node",
                                    text);
    }
    case ANNEAU_MSG_TOPOLOGY_REQ:
        return send_topology_response(state);
    case ANNEAU_MSG_FILE_START_REQ:
    case ANNEAU_MSG_FILE_CHUNK_REQ:
    case ANNEAU_MSG_FILE_END_REQ:
        return echo_file(state, frame);
    default:
        return send_status(state->client_fd, ANNEAU_MSG_ERROR, -1, frame->header.request_id,
                           "Type de message non géré par le mock");
    }
}

int main(int argc, char **argv)
{
    struct driver_state state;
    struct anneau_frame frame;
    int opt = 1;
    int rc = EXIT_SUCCESS;

    memset(&state, 0, sizeof(state));
    anneau_copy_field(state.socket_path, sizeof(state.socket_path), ANNEAU_DEFAULT_SOCKET_PATH);

    while (opt < argc) {
        if (strcmp(argv[opt], "--socket") == 0 && opt + 1 < argc) {
            anneau_copy_field(state.socket_path, sizeof(state.socket_path), argv[opt + 1]);
            opt += 2;
            continue;
        }
        fprintf(stderr, "Usage: %s [--socket chemin_socket]\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    state.listen_fd = create_server_socket(state.socket_path);
    if (state.listen_fd < 0) {
        return EXIT_FAILURE;
    }

    printf("Mock driver en attente sur %s\n", state.socket_path);
    state.client_fd = accept(state.listen_fd, NULL, NULL);
    if (state.client_fd < 0) {
        perror("accept");
        close(state.listen_fd);
        unlink(state.socket_path);
        return EXIT_FAILURE;
    }
    printf("Comm connecté au mock driver.\n");

    while (keep_running) {
        int status = anneau_recv_frame(state.client_fd, &frame);
        if (status <= 0) {
            if (status < 0) {
                perror("anneau_recv_frame");
            }
            break;
        }

        if (handle_frame(&state, &frame) < 0) {
            perror("handle_frame");
            anneau_free_frame(&frame);
            rc = EXIT_FAILURE;
            break;
        }
        anneau_free_frame(&frame);
    }

    close(state.client_fd);
    close(state.listen_fd);
    unlink(state.socket_path);
    return rc;
}
