/*
 * file_crypt.h — Keys and 3DES-CBC encryption for config and field-map files
 *
 * Uses the C library, file_io.h and, in builds with libcrypto
 * (ALLTRAX_HAVE_CRYPTO), OpenSSL. Builds without libcrypto read and write
 * unencrypted files only.
 */

#ifndef ALLTRAX_FILE_CRYPT_H
#define ALLTRAX_FILE_CRYPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A 3DES-CBC key and IV. encrypt is false when files are read and
 * written unencrypted. */
typedef struct {
    bool encrypt;
    unsigned char key[24];
    unsigned char iv[8];
} cli_key;

/* Where a kind of file finds its key for auto. */
typedef struct {
    const char* what;          /* file kind, for messages */
    const char* env;           /* environment variable holding <key>:<iv> */
    const cli_key* embedded;   /* built-in key, or NULL when none is built in */
} cli_key_source;

extern const cli_key_source cli_config_key_source;
extern const cli_key_source cli_field_map_key_source;

/* Resolve a file key from the --key value (NULL means "auto"):
 *   none         unencrypted files
 *   <key>:<iv>   48 and 16 hex digits, used as given
 *   auto         the key in the source's environment variable, else its
 *                built-in key, else an error
 * The environment variable must hold <key>:<iv>; an empty one is ignored.
 * Builds without libcrypto accept only none.
 * Returns 0, or 1 after printing an error naming `command`. */
int cli_resolve_key(const char* command, const char* value,
    const cli_key_source* source, cli_key* out);

/* Print whether encrypted files of this kind are available and whether a
 * key is built in. */
void cli_print_key_status(FILE* out, const cli_key_source* source);

/* Read a file (see cli_read_input()), decrypting it unless key->encrypt is
 * false. Returns a NUL-terminated buffer, or NULL after printing an error. */
char* cli_read_file(const char* path, const cli_key* key, size_t* len);

/* Encrypt data unless key->encrypt is false, then write it (see
 * cli_write_output()). The destination is untouched when encryption fails.
 * Returns 0, or 1 after printing an error. */
int cli_write_file(const char* path, const cli_key* key, const char* data,
    size_t len);

#endif /* ALLTRAX_FILE_CRYPT_H */
