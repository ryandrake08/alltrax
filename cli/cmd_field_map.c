/*
 * cmd_field_map.c — alltrax field-map history|list|upgrade|install
 *
 * history reads the controller's field-map history. list, upgrade and install
 * read a field-map file (libxml2 builds): a version line "Fbbb.ccc.ddd.eee",
 * then records, each introduced by a separator line and holding a map name, an
 * integer version, backslash-separated alias names, an optional flag line
 * (HIDDEN records are ignored), and a config document with the map's settings.
 */

#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"

static int do_history(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "field-map history", 0, 0, 0, &flags) < 0) {
        fprintf(stderr, "Usage: alltrax field-map history\n");
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    if (cli_open(&ctrl, false)) return 1;
    alltrax_field_map_history history;
    alltrax_error err = alltrax_read_field_map_history(ctrl, &history);
    if (err) {
        cli_error(ctrl, err, "reading field-map history");
        alltrax_close(ctrl);
        return 1;
    }
    alltrax_close(ctrl);

    printf("Map count: %" PRIu32 "\n", history.map_count);
    for (size_t i = 0; i < history.record_count; i++) {
        const alltrax_field_map_history_record* record = &history.records[i];
        printf("%u/%u/%04u  %s\n", (unsigned)record->month, (unsigned)record->day,
               (unsigned)record->year, record->name);
    }
    return 0;
}

#ifdef ALLTRAX_HAVE_XML

/* ------------------------------------------------------------------ */
/* Built-in 3DES key and IV (make EMBEDDED_KEYS=0 omits them)         */
/* ------------------------------------------------------------------ */

#ifdef ALLTRAX_EMBEDDED_KEYS
static const cli_key embedded_fieldmap_key = {
    .encrypt = true,
    .key = {
        73, 45, 205, 127, 225, 133, 145, 178, 174, 203,
        252, 8, 253, 19, 8, 108, 120, 51, 118, 17,
        160, 85, 62, 188
    },
    .iv = {
        48, 60, 181, 207, 29, 128, 123, 143
    },
};
#define EMBEDDED_FIELDMAP_KEY (&embedded_fieldmap_key)
#else
#define EMBEDDED_FIELDMAP_KEY NULL
#endif

/* ------------------------------------------------------------------ */
/* Field-map file parsing                                             */
/* ------------------------------------------------------------------ */

#define RECORD_SEPARATOR "----- ----- ----- ----- -----"

typedef struct {
    const char* text;
    size_t len;
} line_ref;

typedef struct {
    line_ref name;
    line_ref aliases;   /* backslash-separated */
    long version;
    const char* xml;
    size_t xml_len;
} map_record;

typedef struct {
    char* text;         /* file contents without carriage returns */
    unsigned version[4];
    map_record* records;
    size_t count;
} map_file;

static bool line_is(line_ref line, const char* text)
{
    return line.len == strlen(text) && memcmp(line.text, text, line.len) == 0;
}

/* Parse a whole decimal int; surrounding whitespace is allowed. */
static bool parse_int(const char* text, size_t len, long* out)
{
    char buf[32];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, text, len);
    buf[len] = '\0';
    char* end;
    errno = 0;
    long value = strtol(buf, &end, 10);
    if (end == buf || errno == ERANGE || value < INT_MIN || value > INT_MAX)
        return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    *out = value;
    return true;
}

static bool alias_matches(line_ref aliases, const char* name)
{
    if (aliases.len == 0)
        return false;
    size_t name_len = strlen(name);
    const char* p = aliases.text;
    const char* end = aliases.text + aliases.len;
    while (p <= end) {
        const char* sep = memchr(p, '\\', (size_t)(end - p));
        if (!sep) sep = end;
        if ((size_t)(sep - p) == name_len && memcmp(p, name, name_len) == 0)
            return true;
        p = sep + 1;
    }
    return false;
}

/* Parse the record whose separator is lines[i]. Returns false for a malformed
 * record; *hidden is set for a HIDDEN record. */
static bool parse_record(const line_ref* lines, size_t count, size_t i,
    map_record* out, bool* hidden)
{
    if (i + 4 >= count || !parse_int(lines[i + 2].text, lines[i + 2].len, &out->version))
        return false;
    out->name = lines[i + 1];
    out->aliases = lines[i + 3];
    bool flag_line = !line_is(lines[i + 4], "<XCT>");
    *hidden = flag_line && line_is(lines[i + 4], "HIDDEN");

    size_t end = i + 1;
    while (end < count && !line_is(lines[end], RECORD_SEPARATOR))
        end++;
    size_t start = i + 4 + (flag_line ? 1 : 0);
    out->xml = start < end ? lines[start].text : "";
    out->xml_len = start < end
        ? (size_t)(lines[end - 1].text + lines[end - 1].len - out->xml) : 0;
    return true;
}

static void free_map_file(map_file* file)
{
    free(file->records);
    free(file->text);
}

/* Read, decrypt and parse a field-map file. Any malformed record rejects the
 * whole file. Returns 0, or 1 after printing an error. */
static int load_map_file(const char* command, const char* path,
    const cli_flags* flags, map_file* out)
{
    memset(out, 0, sizeof(*out));
    cli_key key;
    if (cli_resolve_key(command, flags->key, "ALLTRAX_FIELDMAP_KEY",
                        EMBEDDED_FIELDMAP_KEY, &key))
        return 1;
    size_t len;
    out->text = cli_read_file(path, &key, &len);
    if (!out->text)
        return 1;

    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (out->text[i] != '\r')
            out->text[n++] = out->text[i];
    }
    out->text[n] = '\0';

    size_t line_count = 1;
    for (size_t i = 0; i < n; i++)
        line_count += out->text[i] == '\n';
    line_ref* lines = calloc(line_count, sizeof(*lines));
    out->records = calloc(line_count, sizeof(*out->records));
    if (!lines || !out->records) {
        fprintf(stderr, "Error: out of memory\n");
        free(lines);
        free_map_file(out);
        return 1;
    }
    const char* p = out->text;
    for (size_t l = 0; l < line_count; l++) {
        const char* nl = strchr(p, '\n');
        lines[l] = (line_ref){ p, nl ? (size_t)(nl - p) : strlen(p) };
        p = nl ? nl + 1 : p + lines[l].len;
    }

    const char* v = lines[0].text;
    bool version_ok = lines[0].len >= 16;
    for (int f = 0; version_ok && f < 4; f++) {
        unsigned value = 0;
        for (int d = 0; d < 3; d++) {
            char c = v[1 + f * 4 + d];
            version_ok = version_ok && c >= '0' && c <= '9';
            value = value * 10 + (unsigned)(c - '0');
        }
        out->version[f] = value;
    }
    if (!version_ok) {
        fprintf(stderr, "Error: %s: %s is not a field-map file (bad version line)\n",
                command, path);
        free(lines);
        free_map_file(out);
        return 1;
    }

    for (size_t l = 1; l < line_count; l++) {
        if (!line_is(lines[l], RECORD_SEPARATOR))
            continue;
        bool hidden = false;
        if (!parse_record(lines, line_count, l, &out->records[out->count], &hidden)) {
            fprintf(stderr, "Error: %s: %s has a malformed map record at line %zu\n",
                    command, path, l + 1);
            free(lines);
            free_map_file(out);
            return 1;
        }
        if (!hidden)
            out->count++;
    }
    free(lines);
    if (out->count == 0) {
        fprintf(stderr, "Error: %s: %s contains no field maps\n", command, path);
        free_map_file(out);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Controller map and record selection                                */
/* ------------------------------------------------------------------ */

/* The controller's map: F_Table_Name split at the first "-V" into a base name
 * and version (0 without a suffix). */
typedef struct {
    bool settings_valid;    /* Var_GoodSet == 0 */
    char name[33];
    char base[33];
    long version;
    bool version_valid;
} current_map;

static int read_current_map(alltrax_controller* ctrl, current_map* out)
{
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Var_GoodSet"), alltrax_find_var("F_Table_Name"),
    };
    alltrax_var_value values[2];
    alltrax_error err = alltrax_read_vars(ctrl, vars, 2, values);
    if (err) {
        cli_error(ctrl, err, "reading the field-map name");
        return 1;
    }
    const char* name = values[1].raw.str;
    while (*name == ' ') name++;
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->settings_valid = values[0].raw.u16 == 0;
    out->version = 0;
    out->version_valid = true;
    snprintf(out->base, sizeof(out->base), "%s", out->name);
    char* suffix = strstr(out->base, "-V");
    if (suffix) {
        *suffix = '\0';
        out->version_valid = parse_int(suffix + 2, strlen(suffix + 2), &out->version);
    }
    return 0;
}

/* A newer record for the controller's map: the last record that has the same
 * name and a higher version, or lists the name as an alias. */
static const map_record* find_upgrade(const map_file* file, const current_map* cur)
{
    const map_record* found = NULL;
    for (size_t i = 0; i < file->count; i++) {
        const map_record* rec = &file->records[i];
        if ((line_is(rec->name, cur->base) && rec->version > cur->version) ||
            alias_matches(rec->aliases, cur->base))
            found = rec;
    }
    return found && found->version > cur->version && found->version > 0 ? found : NULL;
}

/* The highest version of the map with this name, else of a map listing it as
 * an alias. */
static const map_record* find_by_name(const map_file* file, const char* name)
{
    const map_record* exact = NULL;
    const map_record* alias = NULL;
    for (size_t i = 0; i < file->count; i++) {
        const map_record* rec = &file->records[i];
        if (line_is(rec->name, name) && (!exact || rec->version > exact->version))
            exact = rec;
        else if (alias_matches(rec->aliases, name) &&
                 (!alias || rec->version > alias->version))
            alias = rec;
    }
    return exact ? exact : alias;
}

/* Parse a record's settings into assignments, leaving out Tach_4_8 unless
 * with_tach. Returns NULL after printing an error. */
static alltrax_assignment* record_items(const map_record* rec, bool with_tach,
    cli_config* config, size_t* count)
{
    if (cli_config_parse(rec->xml, rec->xml_len, config))
        return NULL;
    alltrax_assignment* items = cli_config_assignments(config, count);
    if (!items) {
        cli_config_free(config);
        return NULL;
    }
    const alltrax_var_def* tach = alltrax_find_var("Tach_4_8");
    size_t kept = 0;
    for (size_t i = 0; i < *count; i++) {
        if (with_tach || items[i].var != tach)
            items[kept++] = items[i];
    }
    *count = kept;
    if (kept == 0) {
        fprintf(stderr, "Error: field map %.*s has no settings\n",
                (int)rec->name.len, rec->name.text);
        free(items);
        cli_config_free(config);
        return NULL;
    }
    return items;
}

static const char* item_text(const alltrax_assignment* items, size_t count,
    const char* name)
{
    const alltrax_var_def* var = alltrax_find_var(name);
    for (size_t i = 0; i < count; i++) {
        if (items[i].var == var)
            return items[i].text;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* alltrax field-map list|upgrade|install                             */
/* ------------------------------------------------------------------ */

static void print_key_status(void)
{
    cli_print_key_status(stderr, "field-map", "ALLTRAX_FIELDMAP_KEY", EMBEDDED_FIELDMAP_KEY);
}

static int do_list(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "field-map list", CLI_OPT_KEY, 1, 1,
                       &flags) < 0) {
        fprintf(stderr, "Usage: alltrax field-map list [options] <file>\n");
        cli_print_options(stderr, CLI_OPT_KEY);
        print_key_status();
        return 1;
    }
    map_file file;
    if (load_map_file("field-map list", argv[1], &flags, &file)) return 1;

    alltrax_controller* ctrl = NULL;
    current_map cur;
    if (cli_open(&ctrl, false) || read_current_map(ctrl, &cur)) {
        if (ctrl) alltrax_close(ctrl);
        free_map_file(&file);
        return 1;
    }
    alltrax_close(ctrl);

    printf("Field-map file version %u.%u.%u.%u, %zu maps (* = controller map)\n",
           file.version[0], file.version[1], file.version[2], file.version[3],
           file.count);
    for (size_t i = 0; i < file.count; i++) {
        const map_record* rec = &file.records[i];
        bool current = cur.settings_valid && line_is(rec->name, cur.base);
        printf("%c %-32.*s  version %ld", current ? '*' : ' ',
               (int)rec->name.len, rec->name.text, rec->version);
        if (rec->aliases.len) {
            printf("  aliases: ");
            for (size_t c = 0; c < rec->aliases.len; c++) {
                if (rec->aliases.text[c] == '\\') printf(", ");
                else putchar(rec->aliases.text[c]);
            }
        }
        printf("\n");
    }

    const map_record* upgrade = NULL;
    if (!cur.settings_valid) {
        printf("Controller map: unknown (user settings are not valid)\n");
    } else {
        printf("Controller map: %s%s\n", cur.name,
               cur.version_valid ? "" : " (no valid version after -V)");
        if (cur.version_valid)
            upgrade = find_upgrade(&file, &cur);
    }
    if (upgrade)
        printf("Upgrade available: %.*s version %ld\n", (int)upgrade->name.len,
               upgrade->name.text, upgrade->version);
    else
        printf("No upgrade available\n");
    free_map_file(&file);
    return 0;
}

#define UPGRADE_OPTIONS (CLI_OPTS_WRITE | CLI_OPT_KEY | CLI_OPT_DEBUG_NO_RULES)

static int do_upgrade(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "field-map upgrade", UPGRADE_OPTIONS, 1, 1,
                       &flags) < 0) {
        fprintf(stderr,
            "Usage: alltrax field-map upgrade [options] <file>\n"
            "\n"
            "Installs a newer version of the controller's field map from <file>,\n"
            "keeping the speed sensor pole count (Tach_4_8).\n");
        cli_print_options(stderr, UPGRADE_OPTIONS);
        print_key_status();
        return 1;
    }
    map_file file;
    if (load_map_file("field-map upgrade", argv[1], &flags, &file)) return 1;

    alltrax_controller* ctrl = NULL;
    alltrax_assignment* items = NULL;
    cli_config config = {0};
    int ret = 1;
    current_map cur;
    if (cli_open(&ctrl, true) || read_current_map(ctrl, &cur))
        goto done;
    if (!cur.settings_valid) {
        fprintf(stderr, "Error: field-map upgrade: the controller's user settings "
                "are not valid (Var_GoodSet != 0)\n");
        goto done;
    }
    if (!cur.version_valid) {
        fprintf(stderr, "Error: field-map upgrade: map name %s has no valid "
                "version number after -V\n", cur.name);
        goto done;
    }
    const map_record* rec = find_upgrade(&file, &cur);
    if (!rec) {
        printf("No field-map upgrade for %s in %s\n", cur.name, argv[1]);
        ret = 0;
        goto done;
    }
    printf("Upgrading field map %s to %.*s version %ld\n", cur.name,
           (int)rec->name.len, rec->name.text, rec->version);

    size_t count;
    items = record_items(rec, false, &config, &count);
    if (!items)
        goto done;
    cli_warn_field_map(items, count);
    if (cli_config_write(ctrl, items, count, &flags))
        goto done;
    const char* name = item_text(items, count, "F_Table_Name");
    if (strstr(name ? name : cur.name, "UNIMAP"))
        printf("The universal field map (UNIMAP) is installed. For best performance, "
               "install a field map that matches your motor.\n");
    ret = 0;

done:
    if (ctrl) alltrax_close(ctrl);
    free(items);
    cli_config_free(&config);
    free_map_file(&file);
    return ret;
}

#define INSTALL_OPTIONS (CLI_OPTS_WRITE | CLI_OPT_KEY | CLI_OPT_DEBUG_NO_RULES)

static const char* pole_count(bool eight)
{
    return eight ? "8-pole" : "4-pole";
}

static int do_install(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "field-map install", INSTALL_OPTIONS, 2, 2,
                       &flags) < 0) {
        fprintf(stderr,
            "Usage: alltrax field-map install [options] <file> <name>\n"
            "\n"
            "Installs the named field map from <file>, including its speed sensor\n"
            "pole count (Tach_4_8). <name> may be a map name or one of its aliases.\n");
        cli_print_options(stderr, INSTALL_OPTIONS);
        print_key_status();
        return 1;
    }
    map_file file;
    if (load_map_file("field-map install", argv[1], &flags, &file)) return 1;

    alltrax_controller* ctrl = NULL;
    alltrax_assignment* items = NULL;
    cli_config config = {0};
    int ret = 1;
    const map_record* rec = find_by_name(&file, argv[2]);
    if (!rec) {
        fprintf(stderr, "Error: field-map install: no field map named %s in %s\n",
                argv[2], argv[1]);
        goto done;
    }
    size_t count;
    items = record_items(rec, true, &config, &count);
    if (!items)
        goto done;
    cli_warn_field_map(items, count);
    if (cli_open(&ctrl, true))
        goto done;

    const alltrax_var_def* tach = alltrax_find_var("Tach_4_8");
    for (size_t i = 0; i < count; i++) {
        if (items[i].var != tach)
            continue;
        alltrax_var_value old;
        alltrax_error err = alltrax_read_vars(ctrl, &tach, 1, &old);
        if (err) {
            cli_error(ctrl, err, "reading Tach_4_8");
            goto done;
        }
        fprintf(stderr, "Speed sensor pole count (Tach_4_8): %s -> %s\n",
                pole_count(old.raw.u8 != 0), pole_count(items[i].number != 0));
    }
    printf("Installing field map %.*s version %ld\n", (int)rec->name.len,
           rec->name.text, rec->version);
    ret = cli_config_write(ctrl, items, count, &flags);

done:
    if (ctrl) alltrax_close(ctrl);
    free(items);
    cli_config_free(&config);
    free_map_file(&file);
    return ret;
}

#endif /* ALLTRAX_HAVE_XML */

int cmd_field_map(int argc, char** argv)
{
    const char* sub = argc >= 2 ? argv[1] : "";
    if (strcmp(sub, "history") == 0)
        return do_history(argc - 1, argv + 1);
    if (strcmp(sub, "list") == 0 || strcmp(sub, "upgrade") == 0 ||
        strcmp(sub, "install") == 0) {
#ifdef ALLTRAX_HAVE_XML
        return sub[0] == 'l' ? do_list(argc - 1, argv + 1)
             : sub[0] == 'u' ? do_upgrade(argc - 1, argv + 1)
             : do_install(argc - 1, argv + 1);
#else
        fprintf(stderr, "Error: field-map %s is not available: this build lacks "
                "libxml2\n", sub);
        return 1;
#endif
    }
    fprintf(stderr,
        "Usage: alltrax field-map <command> [options]\n"
        "\n"
        "  history                Read the controller's field-map history\n"
        "  list <file>            List the maps in a field-map file\n"
        "  upgrade <file>         Install a newer version of the controller's map\n"
        "  install <file> <name>  Install a named map (requires --field-map)\n");
    return 1;
}
