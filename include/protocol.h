#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define ANNEAU_MAGIC 0x41524e47u
#define ANNEAU_VERSION 1u

#define ANNEAU_DEFAULT_SOCKET_PATH "/tmp/anneau_driver.sock"
#define ANNEAU_MAX_NODE_ID 32u
#define ANNEAU_MAX_HOST 64u
#define ANNEAU_MAX_FILENAME 128u
#define ANNEAU_MAX_STATUS_TEXT 128u
#define ANNEAU_MAX_FILE_CHUNK 4096u
#define ANNEAU_MAX_TOPOLOGY_ENTRIES 32u
#define ANNEAU_MAX_INCOMING_TRANSFERS 8u

enum anneau_message_type {
    ANNEAU_MSG_HELLO = 1,
    ANNEAU_MSG_ACK = 2,
    ANNEAU_MSG_ERROR = 3,

    ANNEAU_MSG_JOIN_REQ = 10,
    ANNEAU_MSG_LEAVE_REQ = 11,
    ANNEAU_MSG_SEND_TEXT_REQ = 12,
    ANNEAU_MSG_BROADCAST_REQ = 13,
    ANNEAU_MSG_FILE_START_REQ = 14,
    ANNEAU_MSG_FILE_CHUNK_REQ = 15,
    ANNEAU_MSG_FILE_END_REQ = 16,
    ANNEAU_MSG_TOPOLOGY_REQ = 17,

    ANNEAU_MSG_STATUS_EVT = 30,
    ANNEAU_MSG_TEXT_EVT = 31,
    ANNEAU_MSG_BROADCAST_EVT = 32,
    ANNEAU_MSG_FILE_START_EVT = 33,
    ANNEAU_MSG_FILE_CHUNK_EVT = 34,
    ANNEAU_MSG_FILE_END_EVT = 35,
    ANNEAU_MSG_TOPOLOGY_RSP = 36,

    ANNEAU_MSG_RING_TOKEN = 100,
    ANNEAU_MSG_RING_FORWARD = 101
};

struct anneau_frame_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t request_id;
    uint32_t payload_len;
    uint32_t reserved;
};

struct anneau_status_payload {
    int32_t code;
    char message[ANNEAU_MAX_STATUS_TEXT];
};

struct anneau_join_request {
    char node_id[ANNEAU_MAX_NODE_ID];
    char listen_host[ANNEAU_MAX_HOST];
    uint16_t listen_port;
    uint16_t flags;
    char bootstrap_host[ANNEAU_MAX_HOST];
    uint16_t bootstrap_port;
    uint16_t reserved;
};

struct anneau_leave_request {
    char reason[ANNEAU_MAX_STATUS_TEXT];
};

struct anneau_text_request {
    char destination[ANNEAU_MAX_NODE_ID];
    uint32_t text_len;
};

struct anneau_broadcast_request {
    uint32_t text_len;
};

struct anneau_file_start {
    uint32_t transfer_id;
    char peer_id[ANNEAU_MAX_NODE_ID];
    char filename[ANNEAU_MAX_FILENAME];
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t total_chunks;
};

struct anneau_file_chunk {
    uint32_t transfer_id;
    uint32_t chunk_index;
    uint32_t data_len;
};

struct anneau_file_end {
    uint32_t transfer_id;
    uint32_t total_chunks;
    uint64_t file_size;
    int32_t status;
};

struct anneau_topology_request {
    uint32_t max_entries;
};

struct anneau_peer_info {
    char node_id[ANNEAU_MAX_NODE_ID];
    char host[ANNEAU_MAX_HOST];
    uint16_t input_port;
    uint16_t output_port;
    uint16_t state;
    uint16_t reserved;
};

struct anneau_topology_response {
    uint32_t count;
};

struct anneau_text_event {
    char source[ANNEAU_MAX_NODE_ID];
    char destination[ANNEAU_MAX_NODE_ID];
    uint32_t text_len;
};

struct anneau_broadcast_event {
    char source[ANNEAU_MAX_NODE_ID];
    uint32_t text_len;
};

struct anneau_frame {
    struct anneau_frame_header header;
    uint8_t *payload;
};

void anneau_copy_field(char *dst, size_t dst_size, const char *src);
size_t anneau_field_len(const char *field, size_t field_size);

#endif
