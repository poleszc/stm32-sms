#ifndef CYCLIC_BUFFER_H
#define CYCLIC_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buffer;
    size_t write_ix;
    size_t read_ix;
    size_t num_elements; // Number of elements in the buffer.
    size_t max_capacity; // Maximum capacity of the buffer.
} CyclicBuffer_t;

inline void cycBufInit(CyclicBuffer_t *buf, char *inner_buffer, size_t max_size) {
    buf->buffer = inner_buffer;
    buf->max_capacity = max_size;
    buf->write_ix = 0;
    buf->read_ix = 0;
    buf->num_elements = 0;
}

inline void cycBufReset(CyclicBuffer_t *buf) {
    buf->write_ix = 0;
    buf->read_ix = 0;
    buf->num_elements = 0;
}

inline int cycBufWrite(CyclicBuffer_t *buf, char data) {
    if (buf->num_elements >= buf->max_capacity) {
        return -1;
    }

    buf->buffer[buf->write_ix] = data;
    buf->write_ix = (buf->write_ix + 1) % buf->max_capacity;
    buf->num_elements++;

    return 0;
}

inline int cycBufRead(CyclicBuffer_t *buf, char *data) {
    if (buf->num_elements == 0) {
        return -1;
    }

    *data = buf->buffer[buf->read_ix];
    buf->read_ix = (buf->read_ix + 1) % buf->max_capacity;
    buf->num_elements--;
    return 0;
}

inline char cycBufPeek(CyclicBuffer_t *buf, int lookahead) {
    return buf->buffer[(buf->read_ix + lookahead) % buf->max_capacity];
}

inline bool cycBufEmpty(CyclicBuffer_t *buf) { return buf->num_elements == 0; }

inline bool cycBufFull(CyclicBuffer_t *buf) { return buf->num_elements == buf->max_capacity; }

inline size_t cycBufSize(CyclicBuffer_t *buf) { return buf->num_elements; }

inline size_t cycBufCapacity(CyclicBuffer_t *buf) { return buf->max_capacity - buf->num_elements; }

#endif