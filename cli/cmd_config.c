/*
 * cmd_config.c — alltrax config save/load: Backup and restore settings
 *
 * Reads/writes XML config files compatible with the Windows Alltrax
 * Toolkit, using libxml2. With OpenSSL libcrypto (ALLTRAX_HAVE_CRYPTO), files
 * can be 3DES-CBC encrypted; without it, only plain XML files are supported.
 */

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef ALLTRAX_HAVE_CRYPTO
#include <openssl/evp.h>
#endif
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"

/* ------------------------------------------------------------------ */
/* Built-in 3DES key and IV (make EMBEDDED_KEYS=0 omits them)         */
/* ------------------------------------------------------------------ */

#ifdef ALLTRAX_EMBEDDED_KEYS
static const cli_key embedded_config_key = {
    .encrypt = true,
    .key = {
        248, 184, 42, 77, 161, 89, 116, 236, 75, 162,
        82, 159, 46, 236, 212, 89, 30, 199, 23, 166,
        168, 153, 90, 182
    },
    .iv = {
        250, 217, 74, 37, 158, 170, 24, 231
    },
};
#define EMBEDDED_CONFIG_KEY (&embedded_config_key)
#else
#define EMBEDDED_CONFIG_KEY NULL
#endif

static int resolve_config_key(const char* command, const cli_flags* flags,
    cli_key* key)
{
    return cli_resolve_key(command, flags->key, "ALLTRAX_CONFIG_KEY",
                           EMBEDDED_CONFIG_KEY, key);
}

static void print_key_status(FILE* out)
{
    cli_print_key_status(out, "config", "ALLTRAX_CONFIG_KEY", EMBEDDED_CONFIG_KEY);
}

#ifdef ALLTRAX_HAVE_CRYPTO

/* ------------------------------------------------------------------ */
/* 3DES-CBC encrypt / decrypt                                          */
/* ------------------------------------------------------------------ */

static unsigned char* des3_encrypt(const cli_key* key, const char* plain,
    size_t plain_len, size_t* out_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    unsigned char* out = malloc(plain_len + 8);  /* +block for padding */
    if (!out) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int len = 0, total = 0;

    if (!EVP_EncryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, key->key, key->iv))
        goto fail;
    if (!EVP_EncryptUpdate(ctx, out, &len,
                         (const unsigned char*)plain, (int)plain_len))
        goto fail;
    total = len;
    if (!EVP_EncryptFinal_ex(ctx, out + total, &len))
        goto fail;
    total += len;

    EVP_CIPHER_CTX_free(ctx);
    *out_len = (size_t)total;
    return out;

/* OpenSSL failures release both the context and the partial ciphertext. */
fail:
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
}

static char* des3_decrypt(const cli_key* key, const unsigned char* cipher,
    size_t cipher_len, size_t* out_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    char* out = malloc(cipher_len + 1);
    if (!out) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int len = 0, total = 0;

    if (!EVP_DecryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, key->key, key->iv))
        goto fail;
    if (!EVP_DecryptUpdate(ctx, (unsigned char*)out, &len,
                         cipher, (int)cipher_len))
        goto fail;
    total = len;
    if (!EVP_DecryptFinal_ex(ctx, (unsigned char*)out + total, &len))
        goto fail;
    total += len;

    EVP_CIPHER_CTX_free(ctx);
    out[total] = '\0';
    *out_len = (size_t)total;
    return out;

/* OpenSSL failures release both the context and the partial plaintext. */
fail:
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
}
#endif /* ALLTRAX_HAVE_CRYPTO */

/* ------------------------------------------------------------------ */
/* Config save: groups to include                                     */
/* ------------------------------------------------------------------ */

static const alltrax_var_group save_groups[] = {
    ALLTRAX_VARS_VOLTAGE,
    ALLTRAX_VARS_NORMAL_USER,
    ALLTRAX_VARS_USER1,
    ALLTRAX_VARS_USER2,
    ALLTRAX_VARS_TACH,
    ALLTRAX_VARS_OTHER_SETTINGS,
    ALLTRAX_VARS_THROTTLE,
    ALLTRAX_VARS_FIELD,
};
#define NUM_SAVE_GROUPS (sizeof(save_groups) / sizeof(save_groups[0]))

/* ------------------------------------------------------------------ */
/* Curve-to-XML mapping                                               */
/* ------------------------------------------------------------------ */

static const struct {
    const char*       pointer_name;  /* XML element name for X */
    const char*       data_name;     /* XML element name for Y */
    const char*       curve_type;    /* alltrax_find_curve() key */
    alltrax_var_group group;         /* which Node-* to nest under */
} curve_xml_map[] = {
    { "Lin_Table_Pointer", "Lin_Table_Data", "linearization", ALLTRAX_VARS_THROTTLE },
    { "V_Table_Pointer",   "V_Table_Data",   "speed",         ALLTRAX_VARS_THROTTLE },
    { "I_Table_Pointer",   "I_Table_Data",   "torque",        ALLTRAX_VARS_THROTTLE },
    { "F_Table_Pointer",   "F_Table_Data",   "field",         ALLTRAX_VARS_FIELD    },
    { "Gen_Field_Pointer", "Gen_Field_Data", "genfield",      ALLTRAX_VARS_FIELD    },
};
#define NUM_CURVE_XML (sizeof(curve_xml_map) / sizeof(curve_xml_map[0]))

/* ------------------------------------------------------------------ */
/* XML generation (save)                                              */
/* ------------------------------------------------------------------ */

/* Format 16 raw int16 values as comma-separated string into buf.
 * buf must be at least 16*7 = 112 bytes. */
static void format_curve_csv(const double* display, double scale, char* buf,
    size_t bufsize)
{
    size_t pos = 0;
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        int16_t raw = (int16_t)round(display[i] / scale);
        int n = snprintf(buf + pos, bufsize - pos, "%s%d",
                         i > 0 ? "," : "", raw);
        if (n > 0) pos += (size_t)n;
    }
}

static void report_no_memory(void)
{
    fprintf(stderr, "Error: out of memory building config XML\n");
}

static bool saved_settings_valid(alltrax_controller* ctrl)
{
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Var_GoodSet"), alltrax_find_var("Fact_GoodSet"),
    };
    if (!vars[0] || !vars[1]) {
        fprintf(stderr, "Error: GoodSet variable definitions are missing\n");
        return false;
    }
    alltrax_var_value vals[2];
    alltrax_error err = alltrax_read_vars(ctrl, vars, 2, vals);
    if (err) {
        cli_error(ctrl, err, "reading settings validity");
        return false;
    }
    int64_t user = alltrax_var_raw_int64(&vals[0]);
    int64_t factory = alltrax_var_raw_int64(&vals[1]);
    if (user == 0)
        return true;
    fprintf(stderr,
        "Error: cannot save settings: %s (Var_GoodSet=0x%04X, Fact_GoodSet=0x%04X)\n",
        factory == 0 ? "only factory default values are present on the controller"
                     : "no settings loaded into the controller",
        (unsigned)user, (unsigned)factory);
    return false;
}

/* Read every save group and curve into XML. Any read or XML failure
 * prints an error and returns NULL. */
static char* build_xml(alltrax_controller* ctrl, size_t* total_vars,
    size_t* total_curves)
{
    char* result = NULL;
    xmlBufferPtr buf = NULL;
    size_t count = 0, curves = 0;

    /* Track group nodes so we can append curves to them */
    xmlNodePtr group_nodes[ALLTRAX_VARS_GROUP_COUNT] = {0};

    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr root = doc ? xmlNewNode(NULL, BAD_CAST "XCT") : NULL;
    if (!root) {
        report_no_memory();
        goto cleanup;
    }
    xmlDocSetRootElement(doc, root);

    for (size_t g = 0; g < NUM_SAVE_GROUPS; g++) {
        size_t var_count;
        const alltrax_var_def* defs =
            alltrax_get_var_defs(save_groups[g], &var_count);
        const char* gname = alltrax_var_group_name(save_groups[g]);
        if (!defs || var_count == 0) {
            fprintf(stderr, "Error: no variable definitions for %s\n", gname);
            goto cleanup;
        }

        /* Read the writable variables in this group */
        const alltrax_var_def** ptrs =
            calloc(var_count, sizeof(alltrax_var_def*));
        alltrax_var_value* vals =
            calloc(var_count, sizeof(alltrax_var_value));
        if (!ptrs || !vals) {
            free(ptrs);
            free(vals);
            report_no_memory();
            goto cleanup;
        }

        size_t n = 0;
        for (size_t i = 0; i < var_count; i++) {
            if (defs[i].writable)
                ptrs[n++] = &defs[i];
        }

        alltrax_error err = alltrax_read_vars(ctrl, ptrs, n, vals);
        if (err) {
            free(ptrs);
            free(vals);
            char action[96];
            snprintf(action, sizeof(action), "reading %s settings", gname);
            cli_error(ctrl, err, action);
            goto cleanup;
        }

        char node_name[80];
        snprintf(node_name, sizeof(node_name), "Node-%s", gname);
        xmlNodePtr group = xmlNewChild(root, NULL, BAD_CAST node_name, NULL);
        bool ok = group != NULL;

        for (size_t i = 0; ok && i < n; i++) {
            /* Strings are space-padded to the field size. */
            char vbuf[40];
            if (ptrs[i]->type == ALLTRAX_TYPE_STRING) {
                snprintf(vbuf, sizeof(vbuf), "%-*s", (int)ptrs[i]->size,
                         vals[i].raw.str);
            } else {
                int64_t raw = alltrax_var_raw_int64(&vals[i]);
                snprintf(vbuf, sizeof(vbuf), "%" PRId64, raw);
            }
            ok = xmlNewTextChild(group, NULL, BAD_CAST ptrs[i]->name,
                                 BAD_CAST vbuf) != NULL;
        }
        free(ptrs);
        free(vals);
        if (!ok) {
            report_no_memory();
            goto cleanup;
        }
        group_nodes[save_groups[g]] = group;
        count += n;
    }

    /* Append curve data to existing group nodes */
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        xmlNodePtr gnode = group_nodes[curve_xml_map[c].group];
        const alltrax_curve_def* def =
            alltrax_find_curve(curve_xml_map[c].curve_type);
        if (!gnode || !def) {
            fprintf(stderr, "Error: no save group or definition for curve %s\n",
                    curve_xml_map[c].curve_type);
            goto cleanup;
        }

        alltrax_curve_data data;
        alltrax_error err = alltrax_read_curve(ctrl, def, &data);
        if (err) {
            char action[96];
            snprintf(action, sizeof(action), "reading %s curve",
                     curve_xml_map[c].curve_type);
            cli_error(ctrl, err, action);
            goto cleanup;
        }

        char x_csv[128], y_csv[128];
        format_curve_csv(data.x, def->x_scale, x_csv, sizeof(x_csv));
        format_curve_csv(data.y, def->y_scale, y_csv, sizeof(y_csv));
        if (!xmlNewTextChild(gnode, NULL,
                BAD_CAST curve_xml_map[c].pointer_name, BAD_CAST x_csv) ||
            !xmlNewTextChild(gnode, NULL,
                BAD_CAST curve_xml_map[c].data_name, BAD_CAST y_csv)) {
            report_no_memory();
            goto cleanup;
        }
        curves++;
    }

    /* Serialize to string without XML declaration */
    buf = xmlBufferCreate();
    if (!buf || xmlNodeDump(buf, doc, root, 0, 1) < 0) {
        report_no_memory();
        goto cleanup;
    }

    size_t xml_len = (size_t)xmlBufferLength(buf);
    result = malloc(xml_len + 1);
    if (!result) {
        report_no_memory();
        goto cleanup;
    }
    memcpy(result, xmlBufferContent(buf), xml_len);
    result[xml_len] = '\0';
    *total_vars = count;
    *total_curves = curves;

/* All exits release the XML objects; result stays NULL on failure. */
cleanup:
    if (buf) xmlBufferFree(buf);
    if (doc) xmlFreeDoc(doc);
    return result;
}

/* ------------------------------------------------------------------ */
/* XML parsing (load)                                                  */
/* ------------------------------------------------------------------ */

/* X and Y of one curve, collected while parsing */
typedef struct {
    double x[ALLTRAX_CURVE_POINTS];
    double y[ALLTRAX_CURVE_POINTS];
    bool has_x;
    bool has_y;
} curve_points;

/* Parse comma-separated int16 values into display-unit doubles.
 * Returns true on success (exactly 16 values parsed). */
static bool parse_curve_csv(const char* csv, double scale, double* out)
{
    const char* p = csv;
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        char* endptr;
        errno = 0;
        long val = strtol(p, &endptr, 10);
        if (endptr == p || errno == ERANGE) return false;
        if (val < INT16_MIN || val > INT16_MAX) return false;
        out[i] = (double)(int16_t)val * scale;
        p = endptr;
        if (i < ALLTRAX_CURVE_POINTS - 1) {
            if (*p != ',') return false;
            p++;
        }
    }
    /* Allow trailing whitespace/NUL but not extra values */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '\0';
}

/* Parse a whole decimal integer; surrounding whitespace is allowed. */
static bool parse_config_integer(const char* text, long long* out)
{
    char* end;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (end == text || errno == ERANGE)
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != '\0')
        return false;
    *out = value;
    return true;
}

/* Store one curve axis element; returns false for a bad value. */
static bool parse_curve_element(const char* name, const char* value,
    curve_points points[NUM_CURVE_XML], bool* is_curve)
{
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        const alltrax_curve_def* def = alltrax_find_curve(curve_xml_map[c].curve_type);
        bool is_x = strcmp(name, curve_xml_map[c].pointer_name) == 0;
        if (!def || (!is_x && strcmp(name, curve_xml_map[c].data_name) != 0))
            continue;
        *is_curve = true;
        if (!parse_curve_csv(value, is_x ? def->x_scale : def->y_scale,
                             is_x ? points[c].x : points[c].y))
            return false;
        if (is_x)
            points[c].has_x = true;
        else
            points[c].has_y = true;
        return true;
    }
    *is_curve = false;
    return true;
}

/* Add one variable element; returns false for an invalid value. Unknown and
 * read-only variables are skipped. */
static bool parse_var_element(const char* name, const char* value,
    cli_config* out, size_t* cap)
{
    const alltrax_var_def* var = alltrax_find_var(name);
    if (!var) {
        fprintf(stderr, "  Skipping unknown variable: %s\n", name);
        out->skipped++;
        return true;
    }
    /* Select restorable fields from a backup that also contains read-only
     * metadata. The library enforces write permissions. */
    if (!var->writable)
        return true;

    if (out->count >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 128;
        cli_config_entry* tmp = realloc(out->entries, new_cap * sizeof(*tmp));
        if (!tmp) {
            fprintf(stderr, "Error: out of memory\n");
            return false;
        }
        out->entries = tmp;
        *cap = new_cap;
    }

    cli_config_entry* entry = &out->entries[out->count];
    if (var->type == ALLTRAX_TYPE_STRING) {
        if (alltrax_validate_var_string(var, value)) {
            fprintf(stderr, "Error: %s: invalid text (printable ASCII, at most "
                    "%u characters)\n", name, (unsigned)var->size);
            return false;
        }
        *entry = (cli_config_entry){ .var = var };
        snprintf(entry->text, sizeof(entry->text), "%s", value);
        out->count++;
        return true;
    }

    long long raw;
    if (!parse_config_integer(value, &raw) ||
        (var->type == ALLTRAX_TYPE_BOOL && raw != 0 && raw != 1)) {
        fprintf(stderr, "Error: %s: bad value \"%s\"\n", name, value);
        return false;
    }
    *entry = (cli_config_entry){
        .var = var, .number = ((double)raw - var->offset) * var->scale,
    };
    out->count++;
    return true;
}

void cli_config_free(cli_config* config)
{
    free(config->entries);
    config->entries = NULL;
    config->count = 0;
}

int cli_config_parse(const char* xml, size_t len, cli_config* out)
{
    memset(out, 0, sizeof(*out));
    xmlDocPtr doc = xmlReadMemory(xml, (int)len, NULL, NULL, 0);
    if (!doc) {
        fprintf(stderr, "Error: failed to parse config XML\n");
        return 1;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root ||
        (xmlStrcmp(root->name, BAD_CAST "XCT") != 0 &&
         xmlStrcmp(root->name, BAD_CAST "ALL") != 0)) {
        fprintf(stderr, "Error: config file root element is not <XCT>\n");
        xmlFreeDoc(doc);
        return 1;
    }

    curve_points points[NUM_CURVE_XML] = {0};
    size_t cap = 0;
    for (xmlNodePtr group = xmlFirstElementChild(root);
         group; group = xmlNextElementSibling(group)) {
        for (xmlNodePtr elem = xmlFirstElementChild(group);
             elem; elem = xmlNextElementSibling(elem)) {
            const char* name = (const char*)elem->name;
            xmlChar* xval = xmlNodeGetContent(elem);
            if (!xval) continue;
            const char* value = (const char*)xval;
            bool is_curve;
            bool ok = parse_curve_element(name, value, points, &is_curve);
            if (!ok)
                fprintf(stderr, "Error: %s: bad curve data\n", name);
            else if (!is_curve)
                ok = parse_var_element(name, value, out, &cap);
            xmlFree(xval);
            if (!ok)
                goto invalid_config;
        }
    }

    /* Validate every imported curve before opening or applying any settings. */
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        if (!points[c].has_x && !points[c].has_y)
            continue;
        alltrax_curve_data* data = &out->curves[out->curve_count];
        data->def = alltrax_find_curve(curve_xml_map[c].curve_type);
        if (!points[c].has_x || !points[c].has_y) {
            fprintf(stderr, "Error: incomplete %s curve (X and Y required)\n",
                    data->def->name);
            goto invalid_config;
        }
        memcpy(data->x, points[c].x, sizeof(data->x));
        memcpy(data->y, points[c].y, sizeof(data->y));
        if (alltrax_validate_curve(data->def, data)) {
            fprintf(stderr, "Error: invalid %s curve in config\n", data->def->name);
            goto invalid_config;
        }
        out->curve_count++;
    }

    xmlFreeDoc(doc);
    return 0;

/* Reject the whole import and release its entries and XML document. */
invalid_config:
    cli_config_free(out);
    xmlFreeDoc(doc);
    return 1;
}

alltrax_assignment* cli_config_assignments(const cli_config* config,
    size_t* count)
{
    alltrax_assignment* items =
        calloc(config->count + config->curve_count + 1, sizeof(*items));
    if (!items) {
        fprintf(stderr, "Error: out of memory\n");
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < config->count; i++) {
        const cli_config_entry* entry = &config->entries[i];
        items[n++] = entry->var->type == ALLTRAX_TYPE_STRING
            ? (alltrax_assignment){ .var = entry->var, .text = entry->text }
            : (alltrax_assignment){ .var = entry->var, .number = entry->number };
    }
    for (size_t c = 0; c < config->curve_count; c++) {
        items[n++] = (alltrax_assignment){
            .curve = config->curves[c].def, .curve_data = &config->curves[c],
        };
    }
    *count = n;
    return items;
}

int cli_config_write(alltrax_controller* ctrl, alltrax_assignment* items,
    size_t count, const cli_flags* flags)
{
    if (cli_rules_check(ctrl, items, count, NULL, flags))
        return 1;
    alltrax_write_report report;
    alltrax_write_opts write_opts = cli_write_opts(flags, &report);
    char field_map_name[33];
    if (cli_add_field_map_name(ctrl, items, &count, field_map_name))
        return 1;

    /* Settings, strings, and curves go to the device in one batch: one
     * CAL/RUN bracket and one settings page cycle. */
    alltrax_error err = alltrax_write_assignments(ctrl, items, count, &write_opts);
    if (err) {
        cli_error(ctrl, err, "writing settings");
        cli_report_write(ctrl, &report);
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        if (items[i].curve)
            printf("  Wrote curve: %s (%d points)\n",
                   items[i].curve->name, ALLTRAX_CURVE_POINTS);
        else
            printf("  Wrote %s\n", items[i].var->name);
    }
    cli_field_map_written(items, count);
    cli_report_write(ctrl, &report);
    return 0;
}

char* cli_read_file(const char* path, const cli_key* key, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open file: %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 1024 * 1024) {
        fprintf(stderr, "Error: invalid file size\n");
        fclose(f);
        return NULL;
    }

    unsigned char* data = malloc((size_t)file_size + 1);
    if (!data) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(f);
        return NULL;
    }

    size_t nread = fread(data, 1, (size_t)file_size, f);
    fclose(f);
    if (nread != (size_t)file_size) {
        fprintf(stderr, "Error: short read from %s\n", path);
        free(data);
        return NULL;
    }
    data[nread] = '\0';
    *len = nread;

#ifdef ALLTRAX_HAVE_CRYPTO
    if (key->encrypt) {
        char* plain = des3_decrypt(key, data, nread, len);
        bool looks_plain = data[0] == '<' || data[0] == 'F';
        free(data);
        if (!plain)
            fprintf(stderr, "Error: 3DES decryption failed (wrong key or corrupt file)%s\n",
                    looks_plain ? "; the file looks unencrypted, so use --key none" : "");
        return plain;
    }
#else
    (void)key;
#endif
    return (char*)data;
}

#define LOAD_OPTIONS (CLI_OPTS_WRITE | CLI_OPT_KEY | CLI_OPT_THROTTLE | CLI_OPT_FIELD_MAP | \
                      CLI_OPT_DEBUG_NO_RULES)

/* ------------------------------------------------------------------ */
/* alltrax config save <file>                                          */
/* ------------------------------------------------------------------ */

static int do_save(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "config save", CLI_OPT_KEY, 1, 1,
                       &flags) < 0) {
        fprintf(stderr, "Usage: alltrax config save [options] <file>\n");
        cli_print_options(stderr, CLI_OPT_KEY);
        print_key_status(stderr);
        return 1;
    }
    const char* filepath = argv[1];
    cli_key key;
    if (resolve_config_key("config save", &flags, &key)) return 1;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    /* Every read completes before the destination is opened. */
    size_t total_vars = 0, total_curves = 0;
    char* xml = saved_settings_valid(ctrl)
        ? build_xml(ctrl, &total_vars, &total_curves) : NULL;
    alltrax_close(ctrl);

    if (!xml) {
        fprintf(stderr, "Error: config save: no file written\n");
        return 1;
    }

    /* Encrypt before opening the destination so a failure leaves it intact. */
    const void* data = xml;
    size_t data_len = strlen(xml);
    unsigned char* cipher = NULL;
#ifdef ALLTRAX_HAVE_CRYPTO
    if (key.encrypt) {
        cipher = des3_encrypt(&key, xml, data_len, &data_len);
        if (!cipher) {
            free(xml);
            fprintf(stderr, "Error: 3DES encryption failed; no file written\n");
            return 1;
        }
        data = cipher;
    }
#endif

    FILE* f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create file %s: %s\n",
                filepath, strerror(errno));
        free(xml);
        free(cipher);
        return 1;
    }

    /* Buffered write errors such as a full disk surface at fclose(). */
    struct stat st;
    bool regular = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode);
    bool failed = fwrite(data, 1, data_len, f) != data_len;
    int write_errno = failed ? errno : 0;
    if (fclose(f) != 0 && !failed) {
        failed = true;
        write_errno = errno;
    }
    free(xml);
    free(cipher);
    if (failed) {
        /* Only a regular file is removed; device and pipe paths are kept. */
        bool removed = regular && remove(filepath) == 0;
        fprintf(stderr, "Error: writing %s failed: %s%s\n", filepath,
                write_errno ? strerror(write_errno) : "short write",
                removed ? "; partial file removed" : "");
        return 1;
    }

    printf("Saved %zu variables + %zu curves to %s (%s)\n",
           total_vars, total_curves, filepath,
           key.encrypt ? "encrypted" : "plain XML");
    return 0;
}

/* ------------------------------------------------------------------ */
/* alltrax config load [flags] <file>                                  */
/* ------------------------------------------------------------------ */

static int do_load(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "config load", LOAD_OPTIONS, 1, 1,
                       &flags) < 0) {
        fprintf(stderr,
            "Usage: alltrax config load [options] <file>\n"
            "\n"
            "A file with throttle or field-map settings needs --throttle or\n"
            "--field-map to write them.\n");
        cli_print_options(stderr, LOAD_OPTIONS);
        print_key_status(stderr);
        return 1;
    }
    const char* filepath = argv[1];
    cli_key key;
    if (resolve_config_key("config load", &flags, &key)) return 1;

    size_t xml_len;
    char* xml = cli_read_file(filepath, &key, &xml_len);
    if (!xml)
        return 1;
    cli_config config;
    int rc = cli_config_parse(xml, xml_len, &config);
    free(xml);
    if (rc)
        return 1;

    if (config.count == 0 && config.curve_count == 0) {
        fprintf(stderr, "No writable variables found in config file\n");
        cli_config_free(&config);
        return 1;
    }

    printf("Loaded %zu writable variables + %zu curves from %s",
           config.count, config.curve_count, filepath);
    if (config.skipped > 0)
        printf(" (%zu skipped)", config.skipped);
    printf("\n");

    size_t n_items;
    alltrax_assignment* items = cli_config_assignments(&config, &n_items);
    if (!items) {
        cli_config_free(&config);
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    int ret = 1;
    if (n_items == 0) {
        fprintf(stderr, "Error: nothing to write\n");
    } else if (!cli_check_bulk(filepath, items, n_items, &flags)) {
        cli_warn_field_map(items, n_items);
        if (!cli_open(&ctrl, true))
            ret = cli_config_write(ctrl, items, n_items, &flags);
    }

    if (ctrl) alltrax_close(ctrl);
    free(items);
    cli_config_free(&config);
    return ret;
}

/* ------------------------------------------------------------------ */
/* alltrax config <save|load>                                          */
/* ------------------------------------------------------------------ */

int cmd_config(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax config <save|load> [options] <file>\n"
            "\n"
            "  save [options] <file>  Read settings, write config file\n"
            "  load [options] <file>  Load config file, write settings to device\n"
            "\n"
            "Run 'alltrax config save' or 'alltrax config load' for options.\n");
        print_key_status(stderr);
        return 1;
    }

    const char* subcmd = argv[1];

    if (strcmp(subcmd, "save") == 0)
        return do_save(argc - 1, argv + 1);
    if (strcmp(subcmd, "load") == 0)
        return do_load(argc - 1, argv + 1);

    fprintf(stderr, "Unknown config subcommand: %s\n", subcmd);
    return 1;
}
