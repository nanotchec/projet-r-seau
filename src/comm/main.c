#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct incoming_transfer {
    bool active;
    uint32_t transfer_id;
    char source[ANNEAU_MAX_NODE_ID];
    char filename[ANNEAU_MAX_FILENAME];
    uint64_t received_size;
    uint32_t next_chunk;
    FILE *file;
    char output_path[PATH_MAX];
};

struct comm_state {
    int socket_fd;
    char socket_path[PATH_MAX];
    uint32_t next_request_id;
    uint32_t next_transfer_id;
    struct incoming_transfer incoming;
};

static void print_help(void)
{
    printf("Commandes disponibles:\n");
    printf("  help\n");
    printf("  join <id_noeud> <host_ecoute> <port_ecoute> [host_bootstrap port_bootstrap]\n");
    printf("  leave\n");
    printf("  send <destination> <message>\n");
    printf("  broadcast <message>\n");
    printf("  peers\n");
    printf("  sendfile <destination> <chemin_fichier>\n");
    printf("  quit\n");
}

static void trim_newline(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        --len;
    }
}

static int connect_local_socket(const char *socket_path)
{
    int fd = -1;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Chemin de socket trop long: %s\n", socket_path);
        close(fd);
        return -1;
    }
    anneau_copy_field(addr.sun_path, sizeof(addr.sun_path), socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static uint32_t next_request_id(struct comm_state *state)
{
    state->next_request_id += 1;
    if (state->next_request_id == 0) {
        state->next_request_id += 1;
    }
    return state->next_request_id;
}

static uint32_t next_transfer_id(struct comm_state *state)
{
    state->next_transfer_id += 1;
    if (state->next_transfer_id == 0) {
        state->next_transfer_id += 1;
    }
    return state->next_transfer_id;
}

static int send_status_hello(struct comm_state *state)
{
    struct anneau_status_payload hello;

    memset(&hello, 0, sizeof(hello));
    hello.code = 0;
    anneau_copy_field(hello.message, sizeof(hello.message), "COMM_READY");

    return anneau_send_frame(state->socket_fd, ANNEAU_MSG_HELLO, next_request_id(state),
                             &hello, (uint32_t)sizeof(hello));
}

static int send_join(struct comm_state *state, const char *node_id,
                     const char *listen_host, unsigned long listen_port,
                     const char *bootstrap_host, unsigned long bootstrap_port)
{
    struct anneau_join_request join_req;

    if (listen_port > 65535 || bootstrap_port > 65535) {
        fprintf(stderr, "Port invalide.\n");
        return -1;
    }

    memset(&join_req, 0, sizeof(join_req));
    anneau_copy_field(join_req.node_id, sizeof(join_req.node_id), node_id);
    anneau_copy_field(join_req.listen_host, sizeof(join_req.listen_host), listen_host);
    join_req.listen_port = (uint16_t)listen_port;
    if (bootstrap_host != NULL) {
        anneau_copy_field(join_req.bootstrap_host, sizeof(join_req.bootstrap_host), bootstrap_host);
        join_req.bootstrap_port = (uint16_t)bootstrap_port;
    } else {
        join_req.flags = 1;
    }

    return anneau_send_frame(state->socket_fd, ANNEAU_MSG_JOIN_REQ, next_request_id(state),
                             &join_req, (uint32_t)sizeof(join_req));
}

static int send_leave(struct comm_state *state)
{
    struct anneau_leave_request leave_req;

    memset(&leave_req, 0, sizeof(leave_req));
    anneau_copy_field(leave_req.reason, sizeof(leave_req.reason), "Utilisateur");

    return anneau_send_frame(state->socket_fd, ANNEAU_MSG_LEAVE_REQ, next_request_id(state),
                             &leave_req, (uint32_t)sizeof(leave_req));
}

static int send_text(struct comm_state *state, const char *destination, const char *text)
{
    struct anneau_text_request header;
    size_t text_len = strlen(text);
    uint8_t *payload = NULL;
    int rc = -1;

    if (text_len == 0) {
        fprintf(stderr, "Le message ne peut pas être vide.\n");
        return -1;
    }

    payload = malloc(sizeof(header) + text_len);
    if (payload == NULL) {
        perror("malloc");
        return -1;
    }

    memset(&header, 0, sizeof(header));
    anneau_copy_field(header.destination, sizeof(header.destination), destination);
    header.text_len = (uint32_t)text_len;

    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), text, text_len);

    rc = anneau_send_frame(state->socket_fd, ANNEAU_MSG_SEND_TEXT_REQ, next_request_id(state),
                           payload, (uint32_t)(sizeof(header) + text_len));
    free(payload);
    return rc;
}

static int send_broadcast(struct comm_state *state, const char *text)
{
    struct anneau_broadcast_request header;
    size_t text_len = strlen(text);
    uint8_t *payload = NULL;
    int rc = -1;

    if (text_len == 0) {
        fprintf(stderr, "Le message ne peut pas être vide.\n");
        return -1;
    }

    payload = malloc(sizeof(header) + text_len);
    if (payload == NULL) {
        perror("malloc");
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.text_len = (uint32_t)text_len;

    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), text, text_len);

    rc = anneau_send_frame(state->socket_fd, ANNEAU_MSG_BROADCAST_REQ, next_request_id(state),
                           payload, (uint32_t)(sizeof(header) + text_len));
    free(payload);
    return rc;
}

static int send_topology_request(struct comm_state *state)
{
    struct anneau_topology_request request;

    memset(&request, 0, sizeof(request));
    request.max_entries = ANNEAU_MAX_TOPOLOGY_ENTRIES;

    return anneau_send_frame(state->socket_fd, ANNEAU_MSG_TOPOLOGY_REQ, next_request_id(state),
                             &request, (uint32_t)sizeof(request));
}

static int send_file(struct comm_state *state, const char *destination, const char *path)
{
    FILE *file = NULL;
    struct stat st;
    struct anneau_file_start start;
    struct anneau_file_chunk header;
    struct anneau_file_end end;
    uint8_t chunk_payload[sizeof(struct anneau_file_chunk) + ANNEAU_MAX_FILE_CHUNK];
    uint32_t transfer_id = next_transfer_id(state);
    uint32_t chunk_index = 0;
    uint64_t remaining_size = 0;
    int rc = -1;

    if (stat(path, &st) < 0) {
        perror("stat");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Le chemin n'est pas un fichier régulier: %s\n", path);
        return -1;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        perror("fopen");
        return -1;
    }

    memset(&start, 0, sizeof(start));
    start.transfer_id = transfer_id;
    anneau_copy_field(start.peer_id, sizeof(start.peer_id), destination);
    {
        const char *slash = strrchr(path, '/');
        anneau_copy_field(start.filename, sizeof(start.filename), slash ? slash + 1 : path);
    }
    start.file_size = (uint64_t)st.st_size;
    start.chunk_size = ANNEAU_MAX_FILE_CHUNK;
    start.total_chunks = (uint32_t)((start.file_size + ANNEAU_MAX_FILE_CHUNK - 1) / ANNEAU_MAX_FILE_CHUNK);

    if (anneau_send_frame(state->socket_fd, ANNEAU_MSG_FILE_START_REQ, next_request_id(state),
                          &start, (uint32_t)sizeof(start)) < 0) {
        goto cleanup;
    }

    remaining_size = start.file_size;
    while (remaining_size > 0) {
        size_t to_read = remaining_size > ANNEAU_MAX_FILE_CHUNK ? ANNEAU_MAX_FILE_CHUNK : (size_t)remaining_size;
        size_t read_count = fread(chunk_payload + sizeof(header), 1, to_read, file);

        if (read_count != to_read) {
            fprintf(stderr, "Lecture fichier incomplète.\n");
            goto cleanup;
        }

        memset(&header, 0, sizeof(header));
        header.transfer_id = transfer_id;
        header.chunk_index = chunk_index;
        header.data_len = (uint32_t)read_count;
        memcpy(chunk_payload, &header, sizeof(header));

        if (anneau_send_frame(state->socket_fd, ANNEAU_MSG_FILE_CHUNK_REQ,
                              next_request_id(state), chunk_payload,
                              (uint32_t)(sizeof(header) + read_count)) < 0) {
            goto cleanup;
        }

        remaining_size -= read_count;
        chunk_index += 1;
    }

    memset(&end, 0, sizeof(end));
    end.transfer_id = transfer_id;
    end.total_chunks = chunk_index;
    end.file_size = start.file_size;
    end.status = 0;

    rc = anneau_send_frame(state->socket_fd, ANNEAU_MSG_FILE_END_REQ, next_request_id(state),
                           &end, (uint32_t)sizeof(end));

cleanup:
    fclose(file);
    return rc;
}

static void close_transfer(struct incoming_transfer *transfer)
{
    if (transfer->file != NULL) {
        fclose(transfer->file);
        transfer->file = NULL;
    }
    memset(transfer, 0, sizeof(*transfer));
}

static struct incoming_transfer *reserve_transfer(struct comm_state *state, uint32_t transfer_id)
{
    if (state->incoming.active) {
        return NULL;
    }

    state->incoming.active = true;
    state->incoming.transfer_id = transfer_id;
    return &state->incoming;
}

static struct incoming_transfer *find_transfer(struct comm_state *state, uint32_t transfer_id)
{
    if (state->incoming.active && state->incoming.transfer_id == transfer_id) {
        return &state->incoming;
    }

    return NULL;
}

static void ensure_download_dir(void)
{
    if (mkdir("downloads", 0755) < 0 && errno != EEXIST) {
        perror("mkdir downloads");
    }
}

static void build_output_path(char *dst, size_t dst_size, const char *source, const char *filename)
{
    int written = snprintf(dst, dst_size, "downloads/%s_%s", source, filename);
    if (written < 0 || (size_t)written >= dst_size) {
        anneau_copy_field(dst, dst_size, "downloads/received.bin");
    }
}

static void handle_status(const struct anneau_frame *frame)
{
    const struct anneau_status_payload *status = (const struct anneau_status_payload *)frame->payload;

    if (frame->header.payload_len < sizeof(*status)) {
        fprintf(stderr, "Trame status invalide.\n");
        return;
    }

    printf("[driver] code=%d message=%s\n", status->code, status->message);
}

static void handle_text_event(const struct anneau_frame *frame)
{
    const struct anneau_text_event *event = (const struct anneau_text_event *)frame->payload;
    const char *text = NULL;

    if (frame->header.payload_len < sizeof(*event) ||
        frame->header.payload_len < sizeof(*event) + event->text_len) {
        fprintf(stderr, "Trame texte invalide.\n");
        return;
    }

    text = (const char *)(frame->payload + sizeof(*event));
    printf("[message] %s -> %s : %.*s\n", event->source, event->destination,
           (int)event->text_len, text);
}

static void handle_broadcast_event(const struct anneau_frame *frame)
{
    const struct anneau_broadcast_event *event = (const struct anneau_broadcast_event *)frame->payload;
    const char *text = NULL;

    if (frame->header.payload_len < sizeof(*event) ||
        frame->header.payload_len < sizeof(*event) + event->text_len) {
        fprintf(stderr, "Trame diffusion invalide.\n");
        return;
    }

    text = (const char *)(frame->payload + sizeof(*event));
    printf("[diffusion] %s : %.*s\n", event->source, (int)event->text_len, text);
}

static void handle_topology_event(const struct anneau_frame *frame)
{
    const struct anneau_topology_response *response =
        (const struct anneau_topology_response *)frame->payload;
    const struct anneau_peer_info *peer = NULL;
    uint32_t i = 0;

    if (frame->header.payload_len < sizeof(*response)) {
        fprintf(stderr, "Réponse topologie invalide.\n");
        return;
    }

    if (frame->header.payload_len < sizeof(*response) +
            ((size_t)response->count * sizeof(struct anneau_peer_info))) {
        fprintf(stderr, "Réponse topologie tronquée.\n");
        return;
    }

    peer = (const struct anneau_peer_info *)(frame->payload + sizeof(*response));
    printf("Noeud                            Host                             PortE  PortS  Etat\n");
    for (i = 0; i < response->count; ++i) {
        printf("%-32s %-32s %5u %5u %5u\n",
               peer[i].node_id,
               peer[i].host,
               peer[i].input_port,
               peer[i].output_port,
               peer[i].state);
    }
}

static void handle_file_start_event(struct comm_state *state, const struct anneau_frame *frame)
{
    const struct anneau_file_start *start = (const struct anneau_file_start *)frame->payload;
    struct incoming_transfer *transfer = NULL;

    if (frame->header.payload_len < sizeof(*start)) {
        fprintf(stderr, "Début de transfert invalide.\n");
        return;
    }

    ensure_download_dir();
    transfer = reserve_transfer(state, start->transfer_id);
    if (transfer == NULL) {
        fprintf(stderr, "Un seul transfert entrant a la fois est supporte.\n");
        return;
    }

    anneau_copy_field(transfer->source, sizeof(transfer->source), start->peer_id);
    anneau_copy_field(transfer->filename, sizeof(transfer->filename), start->filename);
    build_output_path(transfer->output_path, sizeof(transfer->output_path),
                      transfer->source, transfer->filename);

    transfer->file = fopen(transfer->output_path, "wb");
    if (transfer->file == NULL) {
        perror("fopen");
        close_transfer(transfer);
        return;
    }

    printf("[fichier] réception de %s depuis %s -> %s\n",
           transfer->filename, transfer->source, transfer->output_path);
}

static void handle_file_chunk_event(struct comm_state *state, const struct anneau_frame *frame)
{
    const struct anneau_file_chunk *chunk = (const struct anneau_file_chunk *)frame->payload;
    const uint8_t *data = NULL;
    struct incoming_transfer *transfer = NULL;

    if (frame->header.payload_len < sizeof(*chunk) ||
        frame->header.payload_len < sizeof(*chunk) + chunk->data_len) {
        fprintf(stderr, "Bloc fichier invalide.\n");
        return;
    }

    transfer = find_transfer(state, chunk->transfer_id);
    if (transfer == NULL || transfer->file == NULL) {
        fprintf(stderr, "Transfert inconnu %u.\n", chunk->transfer_id);
        return;
    }

    if (chunk->chunk_index != transfer->next_chunk) {
        fprintf(stderr, "Bloc inattendu pour transfert %u (attendu=%u reçu=%u).\n",
                chunk->transfer_id, transfer->next_chunk, chunk->chunk_index);
        close_transfer(transfer);
        return;
    }

    data = frame->payload + sizeof(*chunk);
    if (fwrite(data, 1, chunk->data_len, transfer->file) != chunk->data_len) {
        fprintf(stderr, "Ecriture fichier impossible.\n");
        close_transfer(transfer);
        return;
    }

    transfer->received_size += chunk->data_len;
    transfer->next_chunk += 1;
}

static void handle_file_end_event(struct comm_state *state, const struct anneau_frame *frame)
{
    const struct anneau_file_end *end = (const struct anneau_file_end *)frame->payload;
    struct incoming_transfer *transfer = NULL;

    if (frame->header.payload_len < sizeof(*end)) {
        fprintf(stderr, "Fin de transfert invalide.\n");
        return;
    }

    transfer = find_transfer(state, end->transfer_id);
    if (transfer == NULL) {
        fprintf(stderr, "Fin de transfert inconnue %u.\n", end->transfer_id);
        return;
    }

    if (transfer->file != NULL) {
        fclose(transfer->file);
        transfer->file = NULL;
    }

    printf("[fichier] transfert %u terminé, %llu octets reçus dans %s\n",
           end->transfer_id,
           (unsigned long long)transfer->received_size,
           transfer->output_path);
    close_transfer(transfer);
}

static int handle_driver_frame(struct comm_state *state)
{
    struct anneau_frame frame;
    int rc = anneau_recv_frame(state->socket_fd, &frame);

    if (rc <= 0) {
        if (rc == 0) {
            fprintf(stderr, "Connexion driver fermée.\n");
        } else {
            perror("anneau_recv_frame");
        }
        return -1;
    }

    switch (frame.header.type) {
    case ANNEAU_MSG_ACK:
    case ANNEAU_MSG_ERROR:
    case ANNEAU_MSG_STATUS_EVT:
        handle_status(&frame);
        break;
    case ANNEAU_MSG_TEXT_EVT:
        handle_text_event(&frame);
        break;
    case ANNEAU_MSG_BROADCAST_EVT:
        handle_broadcast_event(&frame);
        break;
    case ANNEAU_MSG_TOPOLOGY_RSP:
        handle_topology_event(&frame);
        break;
    case ANNEAU_MSG_FILE_START_EVT:
        handle_file_start_event(state, &frame);
        break;
    case ANNEAU_MSG_FILE_CHUNK_EVT:
        handle_file_chunk_event(state, &frame);
        break;
    case ANNEAU_MSG_FILE_END_EVT:
        handle_file_end_event(state, &frame);
        break;
    default:
        printf("[driver] type non géré: %u\n", frame.header.type);
        break;
    }

    anneau_free_frame(&frame);
    return 0;
}

static int handle_command(struct comm_state *state, char *line)
{
    char *argv[8];
    size_t argc = 0;
    char *saveptr = NULL;
    char *token = NULL;
    char *command_end = NULL;

    trim_newline(line);
    command_end = strchr(line, ' ');

    if (strncmp(line, "broadcast ", 10) == 0) {
        return send_broadcast(state, line + 10);
    }

    if (strncmp(line, "send ", 5) == 0) {
        char *destination = line + 5;
        char *message = strchr(destination, ' ');

        if (message == NULL || message[1] == '\0') {
            fprintf(stderr, "Usage: send <destination> <message>\n");
            return 0;
        }
        *message = '\0';
        message += 1;
        return send_text(state, destination, message);
    }

    token = strtok_r(line, " ", &saveptr);
    while (token != NULL && argc < (sizeof(argv) / sizeof(argv[0]))) {
        argv[argc++] = token;
        token = strtok_r(NULL, " ", &saveptr);
    }

    if (argc == 0) {
        return 0;
    }

    if (strcmp(argv[0], "help") == 0) {
        print_help();
        return 0;
    }

    if (strcmp(argv[0], "join") == 0) {
        if (argc != 4 && argc != 6) {
            fprintf(stderr, "Usage: join <id_noeud> <host_ecoute> <port_ecoute> [host_bootstrap port_bootstrap]\n");
            return 0;
        }
        return send_join(state,
                         argv[1],
                         argv[2],
                         strtoul(argv[3], NULL, 10),
                         argc == 6 ? argv[4] : NULL,
                         argc == 6 ? strtoul(argv[5], NULL, 10) : 0);
    }

    if (strcmp(argv[0], "leave") == 0) {
        return send_leave(state);
    }

    if (strcmp(argv[0], "send") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: send <destination> <message>\n");
            return 0;
        }
        return send_text(state, argv[1], argv[2]);
    }

    if (strcmp(argv[0], "broadcast") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: broadcast <message>\n");
            return 0;
        }
        return send_broadcast(state, command_end != NULL ? command_end + 1 : argv[1]);
    }

    if (strcmp(argv[0], "peers") == 0) {
        return send_topology_request(state);
    }

    if (strcmp(argv[0], "sendfile") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: sendfile <destination> <chemin_fichier>\n");
            return 0;
        }
        return send_file(state, argv[1], argv[2]);
    }

    if (strcmp(argv[0], "quit") == 0) {
        return 1;
    }

    fprintf(stderr, "Commande inconnue: %s\n", argv[0]);
    return 0;
}

int main(int argc, char **argv)
{
    struct comm_state state;
    fd_set readfds;
    char *line = NULL;
    size_t line_capacity = 0;
    int max_fd = 0;
    int should_exit = 0;
    int opt = 1;

    memset(&state, 0, sizeof(state));
    anneau_copy_field(state.socket_path, sizeof(state.socket_path), ANNEAU_DEFAULT_SOCKET_PATH);
    state.next_request_id = (uint32_t)time(NULL);
    state.next_transfer_id = state.next_request_id ^ 0x5a5aa5a5u;

    while (opt < argc) {
        if (strcmp(argv[opt], "--socket") == 0 && opt + 1 < argc) {
            anneau_copy_field(state.socket_path, sizeof(state.socket_path), argv[opt + 1]);
            opt += 2;
            continue;
        }

        fprintf(stderr, "Usage: %s [--socket chemin_socket]\n", argv[0]);
        return EXIT_FAILURE;
    }

    state.socket_fd = connect_local_socket(state.socket_path);
    if (state.socket_fd < 0) {
        return EXIT_FAILURE;
    }

    if (send_status_hello(&state) < 0) {
        perror("hello");
        close(state.socket_fd);
        return EXIT_FAILURE;
    }

    printf("Comm connecté au driver sur %s\n", state.socket_path);
    print_help();

    for (;;) {
        printf("comm> ");
        fflush(stdout);

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(state.socket_fd, &readfds);
        max_fd = state.socket_fd > STDIN_FILENO ? state.socket_fd : STDIN_FILENO;

        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(state.socket_fd, &readfds) && handle_driver_frame(&state) < 0) {
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t line_len = getline(&line, &line_capacity, stdin);
            if (line_len < 0) {
                putchar('\n');
                break;
            }
            should_exit = handle_command(&state, line);
            if (should_exit < 0) {
                perror("commande");
            }
            if (should_exit != 0) {
                break;
            }
        }
    }

    free(line);
    close(state.socket_fd);
    return EXIT_SUCCESS;
}
