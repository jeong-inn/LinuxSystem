#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define COMMAND_MAX 4096
#define LINE_MAX_LEN 8192
#define MAX_TOKENS 256
#define MAX_VALUES 256
#define PROMPT "20211426> "

typedef struct {
    char monitoring_path[PATH_MAX];
    pid_t pid;
    char start_time[32];
    char output_path[PATH_MAX];
    int time_interval;
    int max_log_lines; /* -1: none */
    char exclude_paths[MAX_VALUES][PATH_MAX];
    size_t exclude_count;
    char extensions[MAX_VALUES][NAME_MAX + 1];
    size_t extension_count;
    int mode;
} Config;

typedef struct {
    int has_output, has_interval, has_log_limit;
    int has_excludes, has_extensions, has_mode;
    char output_path[PATH_MAX];
    int time_interval, max_log_lines, mode;
    char exclude_paths[MAX_VALUES][PATH_MAX];
    size_t exclude_count;
    char extensions[MAX_VALUES][NAME_MAX + 1];
    size_t extension_count;
} OptionPatch;

typedef struct {
    pid_t pid;
    char path[PATH_MAX];
} DaemonEntry;

typedef struct {
    char path[PATH_MAX];
    char name[NAME_MAX + 1];
    char extension[NAME_MAX + 1];
    time_t mtime;
} FileEntry;

typedef struct {
    FileEntry *items;
    size_t count, capacity;
} FileVector;

static char home_dir[PATH_MAX];
static char state_dir[PATH_MAX];
static char daemon_list_path[PATH_MAX];
static volatile sig_atomic_t daemon_stop = 0;

/* Print the built-in command interface required by the specification. */
static void print_help(void)
{
    printf("Usage:\n");
    printf("  > show\n");
    printf("    <none> : show monitoring daemon process info\n");
    printf("  > add <DIR_PATH> [OPTION]...\n");
    printf("    <none> : add daemon process monitoring the <DIR_PATH> directory\n");
    printf("    -d <OUTPUT_PATH> : specify the output directory\n");
    printf("    -i <TIME_INTERVAL> : set the monitoring interval in seconds\n");
    printf("    -l <MAX_LOG_LINES> : set the maximum number of log lines\n");
    printf("    -x <EXCLUDE_PATH1, EXCLUDE_PATH2, ...> : exclude subdirectories\n");
    printf("    -e <EXTENSION1, EXTENSION2, ...> : select file extensions\n");
    printf("    -m <M> : duplicate mode (1: newest, 2: oldest, 3: skip)\n");
    printf("  > modify <DIR_PATH> [OPTION]...\n");
    printf("    <none> : modify daemon process config monitoring <DIR_PATH>\n");
    printf("    -d <OUTPUT_PATH>\n");
    printf("    -i <TIME_INTERVAL>\n");
    printf("    -l <MAX_LOG_LINES>\n");
    printf("    -x <EXCLUDE_PATH1, EXCLUDE_PATH2, ...>\n");
    printf("    -e <EXTENSION1, EXTENSION2, ...>\n");
    printf("    -m <M>\n");
    printf("  > remove <DIR_PATH>\n");
    printf("  > help\n");
    printf("  > exit\n\n");
}

/* Config, log, and daemon-list access is coordinated with blocking POSIX locks. */
static int lock_file(int fd, short type)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLKW, &lock);
}

static void unlock_file(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    (void)fcntl(fd, F_SETLK, &lock);
}

static int path_is_within(const char *path, const char *parent, int strict)
{
    size_t n = strlen(parent);
    if (strncmp(path, parent, n) != 0)
        return 0;
    if (path[n] == '\0')
        return !strict;
    return (n == 1 && parent[0] == '/') || path[n] == '/';
}

static int paths_overlap(const char *a, const char *b)
{
    return path_is_within(a, b, 0) || path_is_within(b, a, 0);
}

/* Resolve a path and enforce the HOME boundary, type, and access permissions. */
static int validate_existing_directory(const char *input, char *resolved,
                                       int require_write)
{
    struct stat st;
    int mode = R_OK | X_OK | (require_write ? W_OK : 0);
    if (realpath(input, resolved) == NULL) {
        printf("Error: cannot access '%s': %s\n\n", input, strerror(errno));
        return 0;
    }
    if (!path_is_within(resolved, home_dir, 0)) {
        printf("%s is outside the home directory\n\n", input);
        return 0;
    }
    if (stat(resolved, &st) == -1 || !S_ISDIR(st.st_mode)) {
        printf("Error: '%s' is not a directory\n\n", input);
        return 0;
    }
    if (access(resolved, mode) == -1) {
        printf("Error: insufficient permission for '%s'\n\n", input);
        return 0;
    }
    return 1;
}

static int ensure_directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno != EEXIST)
        return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *trim(char *text)
{
    char *end;
    while (isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static int parse_positive_integer(const char *text, int *value)
{
    char *end;
    long number;
    if (text == NULL || *text == '\0')
        return 0;
    errno = 0;
    number = strtol(text, &end, 10);
    if (errno || *end != '\0' || number <= 0 || number > INT_MAX)
        return 0;
    *value = (int)number;
    return 1;
}

static int append_csv_paths(const char *input,
                            char values[MAX_VALUES][PATH_MAX], size_t *count)
{
    char *copy, *save = NULL, *part;
    int ok = 1;
    copy = strdup(input);
    if (!copy) return 0;
    for (part = strtok_r(copy, ",", &save); part;
         part = strtok_r(NULL, ",", &save)) {
        part = trim(part);
        if (!*part || *count >= MAX_VALUES || strlen(part) >= PATH_MAX) {
            ok = 0;
            break;
        }
        strcpy(values[(*count)++], part);
    }
    free(copy);
    return ok;
}

static int append_csv_extensions(const char *input,
                                 char values[MAX_VALUES][NAME_MAX + 1],
                                 size_t *count)
{
    char *copy, *save = NULL, *part;
    int ok = 1;
    copy = strdup(input);
    if (!copy) return 0;
    for (part = strtok_r(copy, ",", &save); part;
         part = strtok_r(NULL, ",", &save)) {
        part = trim(part);
        while (*part == '.')
            part++;
        if (!*part || strchr(part, '/') || *count >= MAX_VALUES || strlen(part) > NAME_MAX) {
            ok = 0;
            break;
        }
        strcpy(values[(*count)++], part);
    }
    free(copy);
    return ok;
}

static int tokenize(char *line, char *tokens[], int maximum)
{
    char *readp = line, *writep = line;
    int count = 0;
    while (*readp) {
        char quote = '\0';
        while (isspace((unsigned char)*readp))
            readp++;
        if (!*readp)
            break;
        if (count == maximum)
            return -1;
        tokens[count++] = writep;
        while (*readp) {
            if (quote) {
                if (*readp == quote) {
                    quote = '\0';
                    readp++;
                    continue;
                }
            } else if (*readp == '\'' || *readp == '"') {
                quote = *readp++;
                continue;
            } else if (isspace((unsigned char)*readp)) {
                readp++;
                break;
            }
            if (*readp == '\\' && readp[1])
                readp++;
            *writep++ = *readp++;
        }
        if (quote)
            return -2;
        *writep++ = '\0';
    }
    return count;
}

/* Parse all add/modify options while remembering which values were supplied. */
static int parse_options(char *tokens[], int count, int start, OptionPatch *options)
{
    int i;
    memset(options, 0, sizeof(*options));
    for (i = start; i < count; i++) {
        const char *opt = tokens[i];
        if (strcmp(opt, "-d") == 0) {
            if (options->has_output || ++i >= count) {
                printf("Error: -d requires one non-duplicate argument\n\n");
                return 0;
            }
            options->has_output = 1;
            if (strlen(tokens[i]) >= PATH_MAX) {
                printf("Error: output path is too long\n\n");
                return 0;
            }
            strcpy(options->output_path, tokens[i]);
        } else if (strcmp(opt, "-i") == 0) {
            if (options->has_interval || ++i >= count ||
                !parse_positive_integer(tokens[i], &options->time_interval)) {
                printf("Error: -i requires a natural number\n\n");
                return 0;
            }
            options->has_interval = 1;
        } else if (strcmp(opt, "-l") == 0) {
            if (options->has_log_limit || ++i >= count ||
                !parse_positive_integer(tokens[i], &options->max_log_lines)) {
                printf("Error: -l requires a natural number\n\n");
                return 0;
            }
            options->has_log_limit = 1;
        } else if (strcmp(opt, "-m") == 0) {
            if (options->has_mode || ++i >= count ||
                !parse_positive_integer(tokens[i], &options->mode) ||
                options->mode > 3) {
                printf("Error: -m requires 1, 2, or 3\n\n");
                return 0;
            }
            options->has_mode = 1;
        } else if (strcmp(opt, "-x") == 0 || strcmp(opt, "-e") == 0) {
            int is_x = strcmp(opt, "-x") == 0;
            int consumed = 0;
            if ((is_x && options->has_excludes) || (!is_x && options->has_extensions)) {
                printf("Error: duplicate option '%s'\n\n", opt);
                return 0;
            }
            if (is_x) options->has_excludes = 1;
            else options->has_extensions = 1;
            while (i + 1 < count && tokens[i + 1][0] != '-') {
                i++;
                consumed = 1;
                if (is_x) {
                    if (!append_csv_paths(tokens[i], options->exclude_paths,
                                          &options->exclude_count)) {
                        printf("Error: invalid -x argument\n\n");
                        return 0;
                    }
                } else if (!append_csv_extensions(tokens[i], options->extensions,
                                                  &options->extension_count)) {
                    printf("Error: invalid -e argument\n\n");
                    return 0;
                }
            }
            if (!consumed) {
                printf("Error: %s requires at least one argument\n\n", opt);
                return 0;
            }
        } else {
            printf("Error: unknown option '%s'\n\n", opt);
            return 0;
        }
    }
    return 1;
}

static int validate_excludes(const char *monitor, OptionPatch *options)
{
    size_t i, j;
    for (i = 0; i < options->exclude_count; i++) {
        char resolved[PATH_MAX];
        if (!validate_existing_directory(options->exclude_paths[i], resolved, 0))
            return 0;
        if (!path_is_within(resolved, monitor, 1)) {
            printf("Error: exclude path '%s' is not a subdirectory of '%s'\n\n",
                   options->exclude_paths[i], monitor);
            return 0;
        }
        strcpy(options->exclude_paths[i], resolved);
    }
    for (i = 0; i < options->exclude_count; i++) {
        for (j = i + 1; j < options->exclude_count; j++) {
            if (paths_overlap(options->exclude_paths[i], options->exclude_paths[j])) {
                printf("Error: exclude paths must not be equal or overlap\n\n");
                return 0;
            }
        }
    }
    return 1;
}

static void config_defaults(Config *config)
{
    memset(config, 0, sizeof(*config));
    config->time_interval = 10;
    config->max_log_lines = -1;
    config->mode = 1;
}

/* Read and write the exact colon-delimited config format under a file lock. */
static int write_config(const Config *config)
{
    char path[PATH_MAX];
    int fd, ok = 0;
    size_t i;
    if (snprintf(path, sizeof(path), "%s/ssu_cleanupd.config", config->monitoring_path) >= (int)sizeof(path))
        return 0;
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1 || lock_file(fd, F_WRLCK) == -1) {
        if (fd != -1) close(fd);
        return 0;
    }
    if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1)
        goto done;
    dprintf(fd, "monitoring_path : %s\n", config->monitoring_path);
    dprintf(fd, "pid : %ld\n", (long)config->pid);
    dprintf(fd, "start_time : %s\n", config->start_time);
    dprintf(fd, "output_path : %s\n", config->output_path);
    dprintf(fd, "time_interval : %d\n", config->time_interval);
    if (config->max_log_lines < 0) dprintf(fd, "max_log_lines : none\n");
    else dprintf(fd, "max_log_lines : %d\n", config->max_log_lines);
    dprintf(fd, "exclude_path : ");
    if (!config->exclude_count) dprintf(fd, "none");
    for (i = 0; i < config->exclude_count; i++)
        dprintf(fd, "%s%s", i ? "," : "", config->exclude_paths[i]);
    dprintf(fd, "\nextension : ");
    if (!config->extension_count) dprintf(fd, "all");
    for (i = 0; i < config->extension_count; i++)
        dprintf(fd, "%s%s", i ? "," : "", config->extensions[i]);
    dprintf(fd, "\nmode : %d\n", config->mode);
    ok = fsync(fd) == 0;
done:
    unlock_file(fd);
    close(fd);
    return ok;
}

static int read_config(const char *monitor, Config *config)
{
    char path[PATH_MAX], *line = NULL;
    size_t line_capacity = 0;
    int fd, sm = 0, sp = 0, ss = 0, so = 0, si = 0, sl = 0, sd = 0;
    FILE *fp;
    config_defaults(config);
    if (snprintf(path, sizeof(path), "%s/ssu_cleanupd.config", monitor) >= (int)sizeof(path))
        return 0;
    fd = open(path, O_RDONLY);
    if (fd == -1 || lock_file(fd, F_RDLCK) == -1) {
        if (fd != -1) close(fd);
        return 0;
    }
    fp = fdopen(fd, "r");
    if (!fp) {
        unlock_file(fd); close(fd); return 0;
    }
    while (getline(&line, &line_capacity, fp) >= 0) {
        char *colon = strchr(line, ':'), *key, *value;
        if (!colon) continue;
        *colon = '\0'; key = trim(line); value = trim(colon + 1);
        if (!strcmp(key, "monitoring_path") && strlen(value) < PATH_MAX) {
            strcpy(config->monitoring_path, value); sm = 1;
        } else if (!strcmp(key, "pid")) {
            char *end; long n = strtol(value, &end, 10);
            if (!*end && n > 0) { config->pid = (pid_t)n; sp = 1; }
        } else if (!strcmp(key, "start_time") && strlen(value) < sizeof(config->start_time)) {
            strcpy(config->start_time, value); ss = 1;
        } else if (!strcmp(key, "output_path") && strlen(value) < PATH_MAX) {
            strcpy(config->output_path, value); so = 1;
        } else if (!strcmp(key, "time_interval")) {
            si = parse_positive_integer(value, &config->time_interval);
        } else if (!strcmp(key, "max_log_lines")) {
            if (!strcmp(value, "none")) { config->max_log_lines = -1; sl = 1; }
            else sl = parse_positive_integer(value, &config->max_log_lines);
        } else if (!strcmp(key, "exclude_path")) {
            if (strcmp(value, "none") && !append_csv_paths(value, config->exclude_paths, &config->exclude_count))
                config->exclude_count = 0;
        } else if (!strcmp(key, "extension")) {
            if (strcmp(value, "all") && !append_csv_extensions(value, config->extensions, &config->extension_count))
                config->extension_count = 0;
        } else if (!strcmp(key, "mode")) {
            sd = parse_positive_integer(value, &config->mode) && config->mode <= 3;
        }
    }
    free(line);
    unlock_file(fd);
    fclose(fp);
    return sm && sp && ss && so && si && sl && sd;
}

static int process_is_alive(pid_t pid)
{
    int saved_errno;
    if (pid <= 0)
        return 0;
    if (kill(pid, 0) == 0)
        return 1;
    saved_errno = errno;
    return saved_errno == EPERM;
}

/* The persistent list uses one "pid,absolute_path" record per daemon. */
static int load_entries(FILE *fp, DaemonEntry **output, size_t *output_count)
{
    DaemonEntry *entries = NULL;
    size_t count = 0, capacity = 0;
    char line[LINE_MAX_LEN];
    if (fseek(fp, 0, SEEK_SET) != 0)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        char *comma = strchr(line, ','), *end;
        long pid;
        if (!comma) continue;
        *comma = '\0'; pid = strtol(line, &end, 10); comma = trim(comma + 1);
        if (*end || pid <= 0 || !*comma || strlen(comma) >= PATH_MAX) continue;
        if (count == capacity) {
            DaemonEntry *grown;
            capacity = capacity ? capacity * 2 : 8;
            grown = realloc(entries, capacity * sizeof(*entries));
            if (!grown) { free(entries); return 0; }
            entries = grown;
        }
        entries[count].pid = (pid_t)pid;
        strcpy(entries[count++].path, comma);
    }
    *output = entries; *output_count = count;
    return 1;
}

static void prune_entries(DaemonEntry *entries, size_t *count)
{
    size_t r, w = 0;
    for (r = 0; r < *count; r++)
        if (process_is_alive(entries[r].pid)) entries[w++] = entries[r];
    *count = w;
}

static int rewrite_entries(FILE *fp, const DaemonEntry *entries, size_t count)
{
    size_t i;
    int fd = fileno(fp);
    if (ftruncate(fd, 0) == -1 || fseek(fp, 0, SEEK_SET)) return 0;
    for (i = 0; i < count; i++) fprintf(fp, "%ld,%s\n", (long)entries[i].pid, entries[i].path);
    return fflush(fp) == 0 && fsync(fd) == 0;
}

static int get_entries(DaemonEntry **entries, size_t *count)
{
    FILE *fp = fopen(daemon_list_path, "r+");
    int fd, ok = 0;
    size_t before;
    if (!fp) return 0;
    fd = fileno(fp);
    if (lock_file(fd, F_WRLCK) == -1) { fclose(fp); return 0; }
    if (!load_entries(fp, entries, count)) goto done;
    before = *count; prune_entries(*entries, count);
    if (before != *count && !rewrite_entries(fp, *entries, *count)) {
        free(*entries); *entries = NULL; goto done;
    }
    ok = 1;
done:
    unlock_file(fd); fclose(fp); return ok;
}

static int monitor_conflicts(const char *path)
{
    DaemonEntry *entries = NULL;
    size_t count = 0, i;
    int result = 0;
    if (!get_entries(&entries, &count)) return -1;
    for (i = 0; i < count; i++) if (paths_overlap(path, entries[i].path)) { result = 1; break; }
    free(entries); return result;
}

static int find_daemon(const char *path, pid_t *pid)
{
    DaemonEntry *entries = NULL;
    size_t count = 0, i;
    int found = 0;
    if (!get_entries(&entries, &count)) return 0;
    for (i = 0; i < count; i++) if (!strcmp(path, entries[i].path)) {
        if (pid) *pid = entries[i].pid;
        found = 1; break;
    }
    free(entries); return found;
}

static int add_entry(pid_t pid, const char *path)
{
    FILE *fp = fopen(daemon_list_path, "r+");
    DaemonEntry *entries = NULL, *grown;
    size_t count = 0, i;
    int fd, ok = 0;
    if (!fp) return 0;
    fd = fileno(fp);
    if (lock_file(fd, F_WRLCK) == -1) { fclose(fp); return 0; }
    if (!load_entries(fp, &entries, &count)) goto done;
    prune_entries(entries, &count);
    for (i = 0; i < count; i++) if (paths_overlap(path, entries[i].path)) goto done;
    grown = realloc(entries, (count + 1) * sizeof(*entries));
    if (!grown) goto done;
    entries = grown; entries[count].pid = pid; strcpy(entries[count++].path, path);
    ok = rewrite_entries(fp, entries, count);
done:
    free(entries); unlock_file(fd); fclose(fp); return ok;
}

static int remove_entry(const char *path, pid_t *pid)
{
    FILE *fp = fopen(daemon_list_path, "r+");
    DaemonEntry *entries = NULL;
    size_t count = 0, i, w = 0;
    int fd, found = 0;
    if (!fp) return 0;
    fd = fileno(fp);
    if (lock_file(fd, F_WRLCK) == -1) { fclose(fp); return 0; }
    if (!load_entries(fp, &entries, &count)) goto done;
    for (i = 0; i < count; i++) {
        if (!strcmp(path, entries[i].path)) { found = 1; *pid = entries[i].pid; }
        else if (process_is_alive(entries[i].pid)) entries[w++] = entries[i];
    }
    if (!rewrite_entries(fp, entries, w)) found = 0;
done:
    free(entries); unlock_file(fd); fclose(fp); return found;
}

static int vector_push(FileVector *vector, const FileEntry *entry)
{
    if (vector->count == vector->capacity) {
        FileEntry *grown;
        size_t new_capacity = vector->capacity ? vector->capacity * 2 : 32;
        grown = realloc(vector->items, new_capacity * sizeof(*vector->items));
        if (!grown) return 0;
        vector->items = grown;
        vector->capacity = new_capacity;
    }
    vector->items[vector->count++] = *entry;
    return 1;
}

static int extension_selected(const Config *config, const char *extension)
{
    size_t i;
    if (!config->extension_count) return 1;
    for (i = 0; i < config->extension_count; i++)
        if (!strcmp(config->extensions[i], extension)) return 1;
    return 0;
}

static int excluded_directory(const Config *config, const char *path)
{
    size_t i;
    for (i = 0; i < config->exclude_count; i++)
        if (path_is_within(path, config->exclude_paths[i], 0)) return 1;
    return 0;
}

static void get_extension(const char *name, char *extension)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) strcpy(extension, "noext");
    else strcpy(extension, dot + 1);
}

/* Recursively collect regular files after applying exclusion and extension filters. */
static int collect_files(const char *directory, const Config *config, FileVector *vector)
{
    DIR *dir;
    struct dirent *item;
    if (excluded_directory(config, directory)) return 1;
    dir = opendir(directory);
    if (!dir) return 0;
    while ((item = readdir(dir))) {
        char path[PATH_MAX];
        struct stat st;
        FileEntry entry;
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, item->d_name) >= (int)sizeof(path)) continue;
        if (lstat(path, &st) == -1) continue;
        if (S_ISDIR(st.st_mode)) { (void)collect_files(path, config, vector); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (!strcmp(item->d_name, "ssu_cleanupd.config") || !strcmp(item->d_name, "ssu_cleanupd.log")) continue;
        memset(&entry, 0, sizeof(entry));
        strcpy(entry.path, path);
        strncpy(entry.name, item->d_name, NAME_MAX); entry.name[NAME_MAX] = '\0';
        get_extension(entry.name, entry.extension);
        if (!extension_selected(config, entry.extension)) continue;
        entry.mtime = st.st_mtime;
        if (!vector_push(vector, &entry)) { closedir(dir); return 0; }
    }
    closedir(dir); return 1;
}

/* Copy with file descriptors, preserving permission bits and timestamps. */
static int copy_file(const char *source, const char *destination)
{
    int in = -1, out = -1, ok = 0;
    struct stat st;
    char buffer[16384];
    ssize_t bytes;
    in = open(source, O_RDONLY);
    if (in == -1 || fstat(in, &st) == -1) goto done;
    out = open(destination, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out == -1) goto done;
    while ((bytes = read(in, buffer, sizeof(buffer))) > 0) {
        ssize_t used = 0;
        while (used < bytes) {
            ssize_t amount = write(out, buffer + used, (size_t)(bytes - used));
            if (amount < 0) { if (errno == EINTR) continue; goto done; }
            used += amount;
        }
    }
    if (bytes < 0 || fchmod(out, st.st_mode & 0777) == -1) goto done;
    {
        struct timespec times[2];
        times[0].tv_sec = st.st_atime;
        times[0].tv_nsec = 0;
        times[1].tv_sec = st.st_mtime;
        times[1].tv_nsec = 0;
        (void)futimens(out, times);
    }
    ok = 1;
done:
    if (in != -1) close(in);
    if (out != -1) close(out);
    if (!ok) unlink(destination);
    return ok;
}

static int append_log(const Config *config, const char *source, const char *destination)
{
    char path[PATH_MAX], stamp[16];
    time_t now = time(NULL);
    struct tm value;
    int fd, ok;
    snprintf(path, sizeof(path), "%s/ssu_cleanupd.log", config->monitoring_path);
    localtime_r(&now, &value); strftime(stamp, sizeof(stamp), "%H:%M:%S", &value);
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd == -1 || lock_file(fd, F_WRLCK) == -1) { if (fd != -1) close(fd); return 0; }
    ok = dprintf(fd, "[%s][%ld][%s][%s]\n", stamp, (long)getpid(), source, destination) >= 0;
    unlock_file(fd); close(fd); return ok;
}

/* Retain only the newest N log records when -l is active. */
static void trim_log(const Config *config)
{
    char path[PATH_MAX], **ring, *line = NULL;
    size_t capacity = 0, count = 0, index = 0, i;
    ssize_t length;
    FILE *fp;
    int fd;
    if (config->max_log_lines < 0) return;
    snprintf(path, sizeof(path), "%s/ssu_cleanupd.log", config->monitoring_path);
    fp = fopen(path, "r+"); if (!fp) return;
    fd = fileno(fp);
    if (lock_file(fd, F_WRLCK) == -1) { fclose(fp); return; }
    ring = calloc((size_t)config->max_log_lines, sizeof(*ring));
    if (!ring) goto done;
    while ((length = getline(&line, &capacity, fp)) >= 0) {
        char *copy = malloc((size_t)length + 1);
        if (!copy) break;
        memcpy(copy, line, (size_t)length + 1);
        free(ring[index]); ring[index] = copy;
        index = (index + 1) % (size_t)config->max_log_lines;
        if (count < (size_t)config->max_log_lines) count++;
    }
    if (ftruncate(fd, 0) == -1 || fseek(fp, 0, SEEK_SET)) goto free_ring;
    for (i = 0; i < count; i++) {
        size_t pos = (index + (size_t)config->max_log_lines - count + i) % (size_t)config->max_log_lines;
        fputs(ring[pos], fp);
    }
    fflush(fp); fsync(fd);
free_ring:
    for (i = 0; i < (size_t)config->max_log_lines; i++) free(ring[i]);
    free(ring);
done:
    free(line); unlock_file(fd); fclose(fp);
}

/* Group equal basenames across the entire tree and apply duplicate mode 1/2/3. */
static void arrange_files(const Config *config)
{
    FileVector vector = {0};
    unsigned char *handled;
    size_t i, j;
    int wrote_log = 0;
    if (!collect_files(config->monitoring_path, config, &vector)) { free(vector.items); return; }
    handled = calloc(vector.count ? vector.count : 1, 1);
    if (!handled) { free(vector.items); return; }
    for (i = 0; i < vector.count; i++) {
        size_t selected = i, duplicates = 0;
        char ext_dir[PATH_MAX], destination[PATH_MAX];
        struct stat dst;
        int exists, should_copy = 1;
        if (handled[i]) continue;
        for (j = i; j < vector.count; j++) if (!strcmp(vector.items[i].name, vector.items[j].name)) {
            handled[j] = 1; duplicates++;
            if ((config->mode == 1 && vector.items[j].mtime > vector.items[selected].mtime) ||
                (config->mode == 2 && vector.items[j].mtime < vector.items[selected].mtime) ||
                (vector.items[j].mtime == vector.items[selected].mtime && strcmp(vector.items[j].path, vector.items[selected].path) < 0))
                selected = j;
        }
        if (snprintf(ext_dir, sizeof(ext_dir), "%s/%s", config->output_path, vector.items[selected].extension) >= (int)sizeof(ext_dir)) continue;
        if (!ensure_directory(ext_dir)) continue;
        if (snprintf(destination, sizeof(destination), "%s/%s", ext_dir, vector.items[selected].name) >= (int)sizeof(destination)) continue;
        exists = stat(destination, &dst) == 0;
        if (config->mode == 3) should_copy = duplicates == 1 && !exists;
        else if (exists) {
            if (!S_ISREG(dst.st_mode)) should_copy = 0;
            else if (config->mode == 1) should_copy = vector.items[selected].mtime > dst.st_mtime;
            else should_copy = vector.items[selected].mtime < dst.st_mtime;
        }
        if (should_copy && copy_file(vector.items[selected].path, destination) &&
            append_log(config, vector.items[selected].path, destination)) wrote_log = 1;
    }
    if (wrote_log) trim_log(config);
    free(handled); free(vector.items);
}

static void stop_daemon(int signal_number)
{
    (void)signal_number; daemon_stop = 1;
}

static void sleep_seconds(int seconds)
{
    struct timespec request = {seconds, 0};
    while (!daemon_stop && nanosleep(&request, &request) == -1 && errno == EINTR) ;
}

/* Detach from the shell, then reload config before every cleanup pass. */
static void daemon_process(const char *monitor, int sync_fd)
{
    struct sigaction action;
    char ready;
    int null_fd;
    umask(0);
    if (setsid() == -1 || chdir("/") == -1) _exit(1);
    memset(&action, 0, sizeof(action)); action.sa_handler = stop_daemon;
    sigemptyset(&action.sa_mask); sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd != -1) {
        dup2(null_fd, 0); dup2(null_fd, 1); dup2(null_fd, 2);
        if (null_fd > 2) close(null_fd);
    }
    if (read(sync_fd, &ready, 1) != 1) { close(sync_fd); _exit(1); }
    close(sync_fd);
    while (!daemon_stop) {
        Config config;
        if (!read_config(monitor, &config)) { sleep_seconds(1); continue; }
        sleep_seconds(config.time_interval);
        if (daemon_stop) break;
        if (read_config(monitor, &config)) arrange_files(&config);
    }
    _exit(0);
}

static void now_string(char *output, size_t size)
{
    time_t now = time(NULL); struct tm value;
    localtime_r(&now, &value); strftime(output, size, "%Y-%m-%d %H:%M:%S", &value);
}

/* Validate the complete request before allowing the child daemon to start. */
static void add_command(char *tokens[], int count)
{
    char monitor[PATH_MAX], log_path[PATH_MAX];
    OptionPatch options;
    Config config;
    int conflict, sync_pipe[2], log_fd;
    pid_t pid;
    if (count < 2) { printf("Usage: add <DIR_PATH> [OPTION]...\n\n"); return; }
    if (!validate_existing_directory(tokens[1], monitor, 1) || !parse_options(tokens, count, 2, &options)) return;
    if (options.has_excludes && !validate_excludes(monitor, &options)) return;
    config_defaults(&config); strcpy(config.monitoring_path, monitor);
    if (options.has_output) {
        if (!validate_existing_directory(options.output_path, config.output_path, 1)) return;
        if (paths_overlap(config.output_path, monitor)) {
            printf("Error: output and monitoring paths must not contain each other\n\n"); return;
        }
    } else {
        if (snprintf(config.output_path, sizeof(config.output_path), "%s_arranged", monitor) >= (int)sizeof(config.output_path) ||
            !path_is_within(config.output_path, home_dir, 0) || !ensure_directory(config.output_path)) {
            printf("Error: cannot create the default output directory\n\n"); return;
        }
    }
    if (options.has_interval) config.time_interval = options.time_interval;
    if (options.has_log_limit) config.max_log_lines = options.max_log_lines;
    if (options.has_mode) config.mode = options.mode;
    if (options.has_excludes) {
        config.exclude_count = options.exclude_count;
        memcpy(config.exclude_paths, options.exclude_paths, sizeof(config.exclude_paths));
    }
    if (options.has_extensions) {
        config.extension_count = options.extension_count;
        memcpy(config.extensions, options.extensions, sizeof(config.extensions));
    }
    conflict = monitor_conflicts(monitor);
    if (conflict) {
        printf(conflict > 0 ? "Error: the path is already monitored or overlaps a monitored path\n\n" : "Error: cannot read the daemon list\n\n");
        return;
    }
    snprintf(log_path, sizeof(log_path), "%s/ssu_cleanupd.log", monitor);
    log_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (log_fd == -1) { printf("Error: cannot create log file: %s\n\n", strerror(errno)); return; }
    close(log_fd);
    if (pipe(sync_pipe) == -1) { perror("pipe"); return; }
    pid = fork();
    if (pid == -1) { perror("fork"); close(sync_pipe[0]); close(sync_pipe[1]); return; }
    if (pid == 0) { close(sync_pipe[1]); daemon_process(monitor, sync_pipe[0]); }
    close(sync_pipe[0]); config.pid = pid; now_string(config.start_time, sizeof(config.start_time));
    if (!write_config(&config) || !add_entry(pid, monitor)) {
        printf("Error: failed to register daemon process\n\n"); kill(pid, SIGTERM); close(sync_pipe[1]); return;
    }
    if (write(sync_pipe[1], "1", 1) != 1) { printf("Error: failed to start daemon process\n\n"); kill(pid, SIGTERM); }
    close(sync_pipe[1]); printf("\n");
}

static void print_config_file(const char *monitor)
{
    char path[PATH_MAX], buffer[4096];
    int fd; ssize_t amount;
    snprintf(path, sizeof(path), "%s/ssu_cleanupd.config", monitor);
    fd = open(path, O_RDONLY);
    if (fd == -1 || lock_file(fd, F_RDLCK) == -1) { if (fd != -1) close(fd); return; }
    while ((amount = read(fd, buffer, sizeof(buffer))) > 0) (void)fwrite(buffer, 1, (size_t)amount, stdout);
    unlock_file(fd); close(fd);
}

static void print_last_logs(const char *monitor)
{
    char path[PATH_MAX], *lines[10] = {0}, *line = NULL;
    size_t capacity = 0, total = 0, i;
    ssize_t length;
    FILE *fp; int fd;
    snprintf(path, sizeof(path), "%s/ssu_cleanupd.log", monitor);
    fp = fopen(path, "r"); if (!fp) return;
    fd = fileno(fp);
    if (lock_file(fd, F_RDLCK) == -1) { fclose(fp); return; }
    while ((length = getline(&line, &capacity, fp)) >= 0) {
        char *copy = malloc((size_t)length + 1); size_t pos = total % 10;
        if (!copy) break;
        memcpy(copy, line, (size_t)length + 1); free(lines[pos]); lines[pos] = copy; total++;
    }
    for (i = total > 10 ? total - 10 : 0; i < total; i++) fputs(lines[i % 10], stdout);
    for (i = 0; i < 10; i++) free(lines[i]);
    free(line); unlock_file(fd); fclose(fp);
}

/* Display the list and then the selected config plus its last ten log lines. */
static void show_command(void)
{
    DaemonEntry *entries = NULL;
    size_t count = 0, i;
    char input[128];
    if (!get_entries(&entries, &count)) { printf("Error: cannot read current_daemon_list\n\n"); return; }
    for (;;) {
        int selection;
        printf("Current working daemon process list\n\n0. exit\n");
        for (i = 0; i < count; i++) printf("%zu. %s\n", i + 1, entries[i].path);
        printf("\nSelect one to see process info : "); fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) { free(entries); return; }
        input[strcspn(input, "\n")] = '\0';
        if (!strcmp(input, "0")) { free(entries); printf("\n"); return; }
        if (!parse_positive_integer(input, &selection) || (size_t)selection > count) {
            printf("Please check your input is valid\n\n"); continue;
        }
        printf("\n1. config detail\n\n"); print_config_file(entries[selection - 1].path);
        printf("\n2. log detail\n\n"); print_last_logs(entries[selection - 1].path); printf("\n");
        free(entries); return;
    }
}

static void apply_options(Config *config, const OptionPatch *options)
{
    if (options->has_output) strcpy(config->output_path, options->output_path);
    if (options->has_interval) config->time_interval = options->time_interval;
    if (options->has_log_limit) config->max_log_lines = options->max_log_lines;
    if (options->has_mode) config->mode = options->mode;
    if (options->has_excludes) {
        config->exclude_count = options->exclude_count;
        memcpy(config->exclude_paths, options->exclude_paths, sizeof(config->exclude_paths));
    }
    if (options->has_extensions) {
        config->extension_count = options->extension_count;
        memcpy(config->extensions, options->extensions, sizeof(config->extensions));
    }
}

/* Preserve unspecified fields and atomically replace only supplied options. */
static void modify_command(char *tokens[], int count)
{
    char monitor[PATH_MAX];
    OptionPatch options;
    Config config;
    if (count < 3) { printf("Usage: modify <DIR_PATH> [OPTION]...\n\n"); return; }
    if (!validate_existing_directory(tokens[1], monitor, 1)) return;
    if (!find_daemon(monitor, NULL)) { printf("Error: '%s' is not being monitored\n\n", tokens[1]); return; }
    if (!read_config(monitor, &config)) { printf("Error: cannot read ssu_cleanupd.config\n\n"); return; }
    if (!parse_options(tokens, count, 2, &options)) return;
    if (options.has_excludes && !validate_excludes(monitor, &options)) return;
    if (options.has_output) {
        char resolved[PATH_MAX];
        if (!validate_existing_directory(options.output_path, resolved, 1)) return;
        if (paths_overlap(resolved, monitor)) { printf("Error: output and monitoring paths must not contain each other\n\n"); return; }
        strcpy(options.output_path, resolved);
    }
    apply_options(&config, &options);
    if (!write_config(&config)) { printf("Error: cannot update ssu_cleanupd.config\n\n"); return; }
    printf("Config modified for %s\n\n", monitor);
}

static void remove_command(char *tokens[], int count)
{
    char monitor[PATH_MAX]; pid_t pid;
    if (count != 2) { printf("Usage: remove <DIR_PATH>\n\n"); return; }
    if (!validate_existing_directory(tokens[1], monitor, 0)) return;
    if (!find_daemon(monitor, &pid)) { printf("Error: '%s' is not being monitored\n\n", tokens[1]); return; }
    if (kill(pid, SIGTERM) == -1 && errno != ESRCH) {
        printf("Error: cannot terminate daemon process %ld: %s\n\n", (long)pid, strerror(errno)); return;
    }
    if (!remove_entry(monitor, &pid)) {
        printf("Error: daemon stopped, but current_daemon_list could not be updated\n\n");
        return;
    }
    printf("Daemon process for '%s' has been terminated\n\n", monitor);
}

static int execute_command(char *command)
{
    char *tokens[MAX_TOKENS];
    int count = tokenize(command, tokens, MAX_TOKENS);
    if (!count) return 1;
    if (count < 0) { printf("Error: invalid or overly long command\n\n"); return 1; }
    if (!strcmp(tokens[0], "exit")) {
        if (count != 1) printf("Usage: exit\n\n"); else return 0;
    } else if (!strcmp(tokens[0], "help")) {
        if (count != 1) printf("Usage: help\n\n"); else print_help();
    } else if (!strcmp(tokens[0], "show")) {
        if (count != 1) printf("Usage: show\n\n"); else show_command();
    } else if (!strcmp(tokens[0], "add")) add_command(tokens, count);
    else if (!strcmp(tokens[0], "modify")) modify_command(tokens, count);
    else if (!strcmp(tokens[0], "remove")) remove_command(tokens, count);
    else print_help();
    return 1;
}

static void initialize(void)
{
    const char *home = getenv("HOME");
    int fd;
    if (!home || !realpath(home, home_dir)) { fprintf(stderr, "Error: cannot resolve HOME\n"); exit(EXIT_FAILURE); }
    if (snprintf(state_dir, sizeof(state_dir), "%s/.ssu_cleanupd", home_dir) >= (int)sizeof(state_dir) || !ensure_directory(state_dir)) {
        fprintf(stderr, "Error: cannot create ~/.ssu_cleanupd\n"); exit(EXIT_FAILURE);
    }
    if (snprintf(daemon_list_path, sizeof(daemon_list_path), "%s/current_daemon_list", state_dir) >= (int)sizeof(daemon_list_path)) {
        fprintf(stderr, "Error: daemon list path is too long\n"); exit(EXIT_FAILURE);
    }
    fd = open(daemon_list_path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) { fprintf(stderr, "Error: cannot create current_daemon_list\n"); exit(EXIT_FAILURE); }
    close(fd);
}

static void prompt_loop(void)
{
    char command[COMMAND_MAX + 2];
    int running = 1;
    while (running) {
        size_t length; int c;
        printf(PROMPT); fflush(stdout);
        if (!fgets(command, sizeof(command), stdin)) break;
        length = strlen(command);
        if (length && command[length - 1] != '\n' && !feof(stdin)) {
            while ((c = getchar()) != '\n' && c != EOF) ;
            printf("Error: command exceeds %d bytes\n\n", COMMAND_MAX); continue;
        }
        command[strcspn(command, "\n")] = '\0';
        running = execute_command(command);
    }
}

int main(int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) { fprintf(stderr, "Usage: ssu_cleanupd\n"); return EXIT_FAILURE; }
    initialize(); prompt_loop(); return EXIT_SUCCESS;
}
