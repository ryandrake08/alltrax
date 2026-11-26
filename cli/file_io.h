/*
 * file_io.h — Whole-file input and output for config and field-map files
 *
 * Uses only the C library.
 */

#ifndef ALLTRAX_FILE_IO_H
#define ALLTRAX_FILE_IO_H

#include <stdbool.h>
#include <stddef.h>

/* Largest file read */
#define CLI_FILE_MAX (1024 * 1024)

/* True for "-", which names standard input or standard output. */
bool cli_is_stdio(const char* path);

/* The input for messages: "standard input" for "-", else the path. */
const char* cli_input_name(const char* path);

/* Read a file, or standard input for "-", of 1 byte to CLI_FILE_MAX.
 * Returns a NUL-terminated buffer, or NULL after printing an error. */
char* cli_read_input(const char* path, size_t* len);

/* Write data to path, or to standard output for "-". Buffered write errors
 * such as a full disk are detected at close (or flush). On a write failure a
 * regular file is removed; device and pipe paths are kept. Returns 0, or 1
 * after printing an error. */
int cli_write_output(const char* path, const void* data, size_t len);

#endif /* ALLTRAX_FILE_IO_H */
