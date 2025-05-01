#ifndef BUFFER_H
#define BUFFER_H

typedef struct buffer
{
    int fd;
    char *filename;
    size_t pint_buf_size;
    // size_t request_size;
} bufferRequest;

#endif