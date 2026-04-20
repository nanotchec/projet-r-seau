#ifndef PROTOCOL_IO_H
#define PROTOCOL_IO_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

int anneau_read_full(int fd, void *buffer, size_t size);
int anneau_write_full(int fd, const void *buffer, size_t size);

int anneau_send_frame(int fd, uint16_t type, uint32_t request_id,
                      const void *payload, uint32_t payload_len);
int anneau_recv_frame(int fd, struct anneau_frame *frame);
void anneau_free_frame(struct anneau_frame *frame);

#endif
