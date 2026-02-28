#include <stdlib.h>
#include <string.h>

#include "ws_frame_reassembly.h"

void ws_frame_reassembly_init(ws_frame_reassembly_t *state)
{
    if (!state)
    {
        return;
    }

    state->buffer = NULL;
    state->expected_len = 0;
    state->received_len = 0;
}

void ws_frame_reassembly_reset(ws_frame_reassembly_t *state)
{
    if (!state)
    {
        return;
    }

    if (state->buffer)
    {
        free(state->buffer);
        state->buffer = NULL;
    }

    state->expected_len = 0;
    state->received_len = 0;
}

bool ws_frame_reassembly_begin(ws_frame_reassembly_t *state, int payload_len)
{
    if (!state || payload_len <= 0)
    {
        return false;
    }

    ws_frame_reassembly_reset(state);

    state->buffer = (char *)malloc(payload_len + 1);
    if (!state->buffer)
    {
        return false;
    }

    state->expected_len = payload_len;
    state->received_len = 0;
    return true;
}

bool ws_frame_reassembly_append(ws_frame_reassembly_t *state, int payload_offset, const char *data_ptr, int data_len, int payload_len)
{
    if (!state || !state->buffer || !data_ptr)
    {
        return false;
    }

    if (state->expected_len != payload_len)
    {
        return false;
    }

    if (payload_offset < 0 || data_len <= 0 || payload_offset + data_len > state->expected_len)
    {
        return false;
    }

    memcpy(state->buffer + payload_offset, data_ptr, data_len);

    int fragment_end = payload_offset + data_len;
    if (fragment_end > state->received_len)
    {
        state->received_len = fragment_end;
    }

    if (state->received_len >= state->expected_len)
    {
        state->buffer[state->expected_len] = 0;
    }

    return true;
}

bool ws_frame_reassembly_is_complete(const ws_frame_reassembly_t *state)
{
    if (!state)
    {
        return false;
    }

    return state->buffer && state->received_len >= state->expected_len;
}

const char *ws_frame_reassembly_data(const ws_frame_reassembly_t *state)
{
    if (!state)
    {
        return NULL;
    }

    return state->buffer;
}
