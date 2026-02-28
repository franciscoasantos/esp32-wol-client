#ifndef WS_FRAME_REASSEMBLY_H
#define WS_FRAME_REASSEMBLY_H

#include <stdbool.h>

typedef struct
{
    char *buffer;
    int expected_len;
    int received_len;
} ws_frame_reassembly_t;

void ws_frame_reassembly_init(ws_frame_reassembly_t *state);
void ws_frame_reassembly_reset(ws_frame_reassembly_t *state);
bool ws_frame_reassembly_begin(ws_frame_reassembly_t *state, int payload_len);
bool ws_frame_reassembly_append(ws_frame_reassembly_t *state, int payload_offset, const char *data_ptr, int data_len, int payload_len);
bool ws_frame_reassembly_is_complete(const ws_frame_reassembly_t *state);
const char *ws_frame_reassembly_data(const ws_frame_reassembly_t *state);

#endif
