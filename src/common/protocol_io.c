#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t anneau_htonll(uint64_t value)
{
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)(value & 0xffffffffu));

    return ((uint64_t)low << 32) | high;
}

static uint64_t anneau_ntohll(uint64_t value)
{
    return anneau_htonll(value);
}

void anneau_copy_field(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;

    if (dst_size == 0) {
        return;
    }

    for (; i + 1 < dst_size && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int anneau_read_full(int fd, void *buffer, size_t size)
{
    uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t read_count = read(fd, cursor, size);
        if (read_count == 0) {
            return 0;
        }
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += (size_t)read_count;
        size -= (size_t)read_count;
    }

    return 1;
}

int anneau_write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t write_count = write(fd, cursor, size);
        if (write_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += (size_t)write_count;
        size -= (size_t)write_count;
    }

    return 0;
}

static void anneau_header_to_network(struct anneau_frame_header *dst,
                                     const struct anneau_frame_header *src)
{
    dst->magic = htonl(src->magic);
    dst->version = htons(src->version);
    dst->type = htons(src->type);
    dst->request_id = htonl(src->request_id);
    dst->payload_len = htonl(src->payload_len);
    dst->reserved = htonl(src->reserved);
}

static void anneau_header_from_network(struct anneau_frame_header *dst,
                                       const struct anneau_frame_header *src)
{
    dst->magic = ntohl(src->magic);
    dst->version = ntohs(src->version);
    dst->type = ntohs(src->type);
    dst->request_id = ntohl(src->request_id);
    dst->payload_len = ntohl(src->payload_len);
    dst->reserved = ntohl(src->reserved);
}

static void anneau_payload_to_host(uint16_t type, uint8_t *payload, uint32_t payload_len)
{
    switch (type) {
    case ANNEAU_MSG_ACK:
    case ANNEAU_MSG_ERROR:
    case ANNEAU_MSG_STATUS_EVT: {
        struct anneau_status_payload *status = (struct anneau_status_payload *)payload;
        if (payload_len >= sizeof(*status)) {
            status->code = (int32_t)ntohl((uint32_t)status->code);
        }
        break;
    }
    case ANNEAU_MSG_JOIN_REQ: {
        struct anneau_join_request *join = (struct anneau_join_request *)payload;
        if (payload_len >= sizeof(*join)) {
            join->listen_port = ntohs(join->listen_port);
            join->flags = ntohs(join->flags);
            join->bootstrap_port = ntohs(join->bootstrap_port);
            join->reserved = ntohs(join->reserved);
        }
        break;
    }
    case ANNEAU_MSG_SEND_TEXT_REQ: {
        struct anneau_text_request *text = (struct anneau_text_request *)payload;
        if (payload_len >= sizeof(*text)) {
            text->text_len = ntohl(text->text_len);
        }
        break;
    }
    case ANNEAU_MSG_BROADCAST_REQ: {
        struct anneau_broadcast_request *broadcast = (struct anneau_broadcast_request *)payload;
        if (payload_len >= sizeof(*broadcast)) {
            broadcast->text_len = ntohl(broadcast->text_len);
        }
        break;
    }
    case ANNEAU_MSG_FILE_START_REQ:
    case ANNEAU_MSG_FILE_START_EVT: {
        struct anneau_file_start *file_start = (struct anneau_file_start *)payload;
        if (payload_len >= sizeof(*file_start)) {
            file_start->transfer_id = ntohl(file_start->transfer_id);
            file_start->file_size = anneau_ntohll(file_start->file_size);
            file_start->chunk_size = ntohl(file_start->chunk_size);
            file_start->total_chunks = ntohl(file_start->total_chunks);
        }
        break;
    }
    case ANNEAU_MSG_FILE_CHUNK_REQ:
    case ANNEAU_MSG_FILE_CHUNK_EVT: {
        struct anneau_file_chunk *chunk = (struct anneau_file_chunk *)payload;
        if (payload_len >= sizeof(*chunk)) {
            chunk->transfer_id = ntohl(chunk->transfer_id);
            chunk->chunk_index = ntohl(chunk->chunk_index);
            chunk->data_len = ntohl(chunk->data_len);
        }
        break;
    }
    case ANNEAU_MSG_FILE_END_REQ:
    case ANNEAU_MSG_FILE_END_EVT: {
        struct anneau_file_end *file_end = (struct anneau_file_end *)payload;
        if (payload_len >= sizeof(*file_end)) {
            file_end->transfer_id = ntohl(file_end->transfer_id);
            file_end->total_chunks = ntohl(file_end->total_chunks);
            file_end->file_size = anneau_ntohll(file_end->file_size);
            file_end->status = (int32_t)ntohl((uint32_t)file_end->status);
        }
        break;
    }
    case ANNEAU_MSG_TOPOLOGY_REQ: {
        struct anneau_topology_request *request = (struct anneau_topology_request *)payload;
        if (payload_len >= sizeof(*request)) {
            request->max_entries = ntohl(request->max_entries);
        }
        break;
    }
    case ANNEAU_MSG_TOPOLOGY_RSP: {
        struct anneau_topology_response *response = (struct anneau_topology_response *)payload;
        struct anneau_peer_info *peer = NULL;
        uint32_t i = 0;

        if (payload_len < sizeof(*response)) {
            break;
        }

        response->count = ntohl(response->count);
        peer = (struct anneau_peer_info *)(payload + sizeof(*response));
        for (i = 0; i < response->count; ++i) {
            size_t offset = sizeof(*response) + ((size_t)i * sizeof(*peer));
            if (offset + sizeof(*peer) > payload_len) {
                break;
            }
            peer[i].input_port = ntohs(peer[i].input_port);
            peer[i].output_port = ntohs(peer[i].output_port);
            peer[i].state = ntohs(peer[i].state);
            peer[i].reserved = ntohs(peer[i].reserved);
        }
        break;
    }
    case ANNEAU_MSG_TEXT_EVT: {
        struct anneau_text_event *event = (struct anneau_text_event *)payload;
        if (payload_len >= sizeof(*event)) {
            event->text_len = ntohl(event->text_len);
        }
        break;
    }
    case ANNEAU_MSG_BROADCAST_EVT: {
        struct anneau_broadcast_event *event = (struct anneau_broadcast_event *)payload;
        if (payload_len >= sizeof(*event)) {
            event->text_len = ntohl(event->text_len);
        }
        break;
    }
    default:
        break;
    }
}

static void anneau_payload_to_network(uint16_t type, uint8_t *payload, uint32_t payload_len)
{
    switch (type) {
    case ANNEAU_MSG_ACK:
    case ANNEAU_MSG_ERROR:
    case ANNEAU_MSG_STATUS_EVT: {
        struct anneau_status_payload *status = (struct anneau_status_payload *)payload;
        if (payload_len >= sizeof(*status)) {
            status->code = (int32_t)htonl((uint32_t)status->code);
        }
        break;
    }
    case ANNEAU_MSG_JOIN_REQ: {
        struct anneau_join_request *join = (struct anneau_join_request *)payload;
        if (payload_len >= sizeof(*join)) {
            join->listen_port = htons(join->listen_port);
            join->flags = htons(join->flags);
            join->bootstrap_port = htons(join->bootstrap_port);
            join->reserved = htons(join->reserved);
        }
        break;
    }
    case ANNEAU_MSG_SEND_TEXT_REQ: {
        struct anneau_text_request *text = (struct anneau_text_request *)payload;
        if (payload_len >= sizeof(*text)) {
            text->text_len = htonl(text->text_len);
        }
        break;
    }
    case ANNEAU_MSG_BROADCAST_REQ: {
        struct anneau_broadcast_request *broadcast = (struct anneau_broadcast_request *)payload;
        if (payload_len >= sizeof(*broadcast)) {
            broadcast->text_len = htonl(broadcast->text_len);
        }
        break;
    }
    case ANNEAU_MSG_FILE_START_REQ:
    case ANNEAU_MSG_FILE_START_EVT: {
        struct anneau_file_start *file_start = (struct anneau_file_start *)payload;
        if (payload_len >= sizeof(*file_start)) {
            file_start->transfer_id = htonl(file_start->transfer_id);
            file_start->file_size = anneau_htonll(file_start->file_size);
            file_start->chunk_size = htonl(file_start->chunk_size);
            file_start->total_chunks = htonl(file_start->total_chunks);
        }
        break;
    }
    case ANNEAU_MSG_FILE_CHUNK_REQ:
    case ANNEAU_MSG_FILE_CHUNK_EVT: {
        struct anneau_file_chunk *chunk = (struct anneau_file_chunk *)payload;
        if (payload_len >= sizeof(*chunk)) {
            chunk->transfer_id = htonl(chunk->transfer_id);
            chunk->chunk_index = htonl(chunk->chunk_index);
            chunk->data_len = htonl(chunk->data_len);
        }
        break;
    }
    case ANNEAU_MSG_FILE_END_REQ:
    case ANNEAU_MSG_FILE_END_EVT: {
        struct anneau_file_end *file_end = (struct anneau_file_end *)payload;
        if (payload_len >= sizeof(*file_end)) {
            file_end->transfer_id = htonl(file_end->transfer_id);
            file_end->total_chunks = htonl(file_end->total_chunks);
            file_end->file_size = anneau_htonll(file_end->file_size);
            file_end->status = (int32_t)htonl((uint32_t)file_end->status);
        }
        break;
    }
    case ANNEAU_MSG_TOPOLOGY_REQ: {
        struct anneau_topology_request *request = (struct anneau_topology_request *)payload;
        if (payload_len >= sizeof(*request)) {
            request->max_entries = htonl(request->max_entries);
        }
        break;
    }
    case ANNEAU_MSG_TOPOLOGY_RSP: {
        struct anneau_topology_response *response = (struct anneau_topology_response *)payload;
        struct anneau_peer_info *peer = NULL;
        uint32_t i = 0;

        if (payload_len < sizeof(*response)) {
            break;
        }

        peer = (struct anneau_peer_info *)(payload + sizeof(*response));
        for (i = 0; i < response->count; ++i) {
            size_t offset = sizeof(*response) + ((size_t)i * sizeof(*peer));
            if (offset + sizeof(*peer) > payload_len) {
                break;
            }
            peer[i].input_port = htons(peer[i].input_port);
            peer[i].output_port = htons(peer[i].output_port);
            peer[i].state = htons(peer[i].state);
            peer[i].reserved = htons(peer[i].reserved);
        }
        response->count = htonl(response->count);
        break;
    }
    case ANNEAU_MSG_TEXT_EVT: {
        struct anneau_text_event *event = (struct anneau_text_event *)payload;
        if (payload_len >= sizeof(*event)) {
            event->text_len = htonl(event->text_len);
        }
        break;
    }
    case ANNEAU_MSG_BROADCAST_EVT: {
        struct anneau_broadcast_event *event = (struct anneau_broadcast_event *)payload;
        if (payload_len >= sizeof(*event)) {
            event->text_len = htonl(event->text_len);
        }
        break;
    }
    default:
        break;
    }
}

int anneau_send_frame(int fd, uint16_t type, uint32_t request_id,
                      const void *payload, uint32_t payload_len)
{
    struct anneau_frame_header host_header;
    struct anneau_frame_header network_header;
    uint8_t *buffer = NULL;

    host_header.magic = ANNEAU_MAGIC;
    host_header.version = ANNEAU_VERSION;
    host_header.type = type;
    host_header.request_id = request_id;
    host_header.payload_len = payload_len;
    host_header.reserved = 0;

    anneau_header_to_network(&network_header, &host_header);
    if (anneau_write_full(fd, &network_header, sizeof(network_header)) < 0) {
        return -1;
    }

    if (payload_len == 0 || payload == NULL) {
        return 0;
    }

    buffer = malloc(payload_len);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(buffer, payload, payload_len);
    anneau_payload_to_network(type, buffer, payload_len);

    if (anneau_write_full(fd, buffer, payload_len) < 0) {
        free(buffer);
        return -1;
    }

    free(buffer);
    return 0;
}

int anneau_recv_frame(int fd, struct anneau_frame *frame)
{
    struct anneau_frame_header network_header;
    int read_status = 0;

    memset(frame, 0, sizeof(*frame));

    read_status = anneau_read_full(fd, &network_header, sizeof(network_header));
    if (read_status <= 0) {
        return read_status;
    }

    anneau_header_from_network(&frame->header, &network_header);
    if (frame->header.magic != ANNEAU_MAGIC || frame->header.version != ANNEAU_VERSION) {
        errno = EPROTO;
        return -1;
    }

    if (frame->header.payload_len == 0) {
        return 1;
    }

    frame->payload = malloc(frame->header.payload_len);
    if (frame->payload == NULL) {
        errno = ENOMEM;
        return -1;
    }

    read_status = anneau_read_full(fd, frame->payload, frame->header.payload_len);
    if (read_status <= 0) {
        anneau_free_frame(frame);
        return read_status;
    }

    anneau_payload_to_host(frame->header.type, frame->payload, frame->header.payload_len);
    return 1;
}

void anneau_free_frame(struct anneau_frame *frame)
{
    free(frame->payload);
    frame->payload = NULL;
    memset(&frame->header, 0, sizeof(frame->header));
}
