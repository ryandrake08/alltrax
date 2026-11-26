/*
 * file_io.c — Whole-file input and output for config and field-map files
 */

#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "file_io.h"

bool cli_is_stdio(const char* path)
{
    return strcmp(path, "-") == 0;
}

const char* cli_input_name(const char* path)
{
    return cli_is_stdio(path) ? "standard input" : path;
}

char* cli_read_input(const char* path, size_t* len)
{
    bool std = cli_is_stdio(path);
    FILE* f = std ? stdin : fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open file: %s\n", path);
        return NULL;
    }

    /* One byte more than the limit tells a full-size file from a larger one. */
    char* data = malloc(CLI_FILE_MAX + 2);
    size_t n = data ? fread(data, 1, CLI_FILE_MAX + 1, f) : 0;
    int read_errno = errno;
    bool failed = data && ferror(f);
    if (std)
        clearerr(f);
    else
        fclose(f);
    if (!data) {
        fprintf(stderr, "Error: out of memory\n");
        return NULL;
    }
    if (failed) {
        fprintf(stderr, "Error: reading %s failed: %s\n", cli_input_name(path),
                strerror(read_errno));
        free(data);
        return NULL;
    }
    if (n == 0 || n > CLI_FILE_MAX) {
        fprintf(stderr, "Error: %s is empty or larger than 1 MiB\n",
                cli_input_name(path));
        free(data);
        return NULL;
    }
    data[n] = '\0';
    *len = n;
    return data;
}

int cli_write_output(const char* path, const void* data, size_t len)
{
    if (cli_is_stdio(path)) {
        if (fwrite(data, 1, len, stdout) != len || fflush(stdout) != 0) {
            fprintf(stderr, "Error: writing standard output failed: %s\n",
                    strerror(errno));
            return 1;
        }
        return 0;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create file %s: %s\n",
                path, strerror(errno));
        return 1;
    }

    /* Buffered write errors such as a full disk surface at fclose(). */
    struct stat st;
    bool regular = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode);
    bool failed = fwrite(data, 1, len, f) != len;
    int write_errno = failed ? errno : 0;
    if (fclose(f) != 0 && !failed) {
        failed = true;
        write_errno = errno;
    }
    if (failed) {
        /* Only a regular file is removed; device and pipe paths are kept. */
        bool removed = regular && remove(path) == 0;
        fprintf(stderr, "Error: writing %s failed: %s%s\n", path,
                write_errno ? strerror(write_errno) : "short write",
                removed ? "; partial file removed" : "");
        return 1;
    }
    return 0;
}
