#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PROMPT "20211426> "
#define COMMAND_MAX 4096
#define MAX_TOKENS 256
#define EXT2_SUPER_OFFSET 1024
#define EXT2_SUPER_SIZE 1024
#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_ROOT_INODE 2
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_GROUP_DESC_SIZE 32
#define EXT2_NAME_LEN 255
#define EXT2_NDIR_BLOCKS 12
#define EXT2_N_BLOCKS 15
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define MAX_TREE_DEPTH PATH_MAX

#define EXT2_S_IFMT 0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK 0xA000
#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFBLK 0x6000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFCHR 0x2000
#define EXT2_S_IFIFO 0x1000

typedef struct {
    uint16_t mode;
    uint32_t size_low;
    uint32_t size_high;
    uint32_t blocks[EXT2_N_BLOCKS];
} Ext2Inode;

typedef struct {
    int fd;
    off_t image_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t first_data_block;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint16_t inode_size;
    uint32_t groups_count;
    uint32_t *inode_tables;
} Ext2Fs;

typedef struct EntryNode {
    uint32_t inode;
    uint8_t file_type;
    char name[EXT2_NAME_LEN + 1];
    struct EntryNode *next;
} EntryNode;

typedef struct {
    EntryNode *head;
    size_t count;
} EntryList;

typedef struct {
    uint64_t directories;
    uint64_t files;
} TreeCount;

static uint16_t read_le16(const unsigned char *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* Read an exact byte range and reject offsets outside the image. */
static int image_read(const Ext2Fs *fs, uint64_t offset, void *buffer, size_t size)
{
    unsigned char *cursor = buffer;
    size_t completed = 0;

    if (offset > (uint64_t)fs->image_size ||
        size > (uint64_t)fs->image_size - offset ||
        offset > (uint64_t)INT64_MAX) {
        errno = EIO;
        return 0;
    }
    while (completed < size) {
        ssize_t amount = pread(fs->fd, cursor + completed, size - completed,
                               (off_t)(offset + completed));
        if (amount < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (amount == 0) {
            errno = EIO;
            return 0;
        }
        completed += (size_t)amount;
    }
    return 1;
}

static uint32_t ceil_div_u32(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0);
}

/* Parse only documented on-disk bytes; no ext2 header or library is used. */
static int ext2_open(Ext2Fs *fs, const char *image_path)
{
    unsigned char super[EXT2_SUPER_SIZE];
    struct stat st;
    uint32_t log_block_size;
    uint32_t revision;
    uint32_t feature_incompat;
    uint32_t block_groups;
    uint32_t inode_groups;
    uint64_t descriptor_offset;
    uint64_t represented_size;
    uint32_t group;

    memset(fs, 0, sizeof(*fs));
    fs->fd = -1;
    fs->fd = open(image_path, O_RDONLY);
    if (fs->fd == -1) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", image_path, strerror(errno));
        return 0;
    }
    if (fstat(fs->fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular image file\n", image_path);
        goto fail;
    }
    fs->image_size = st.st_size;
    if (fs->image_size < EXT2_SUPER_OFFSET + EXT2_SUPER_SIZE ||
        !image_read(fs, EXT2_SUPER_OFFSET, super, sizeof(super))) {
        fprintf(stderr, "Error: image is too small to contain an ext2 superblock\n");
        goto fail;
    }
    if (read_le16(super + 56) != EXT2_SUPER_MAGIC) {
        fprintf(stderr, "Error: '%s' is not a valid ext2 image\n", image_path);
        goto fail;
    }

    fs->inodes_count = read_le32(super + 0);
    fs->blocks_count = read_le32(super + 4);
    fs->first_data_block = read_le32(super + 20);
    log_block_size = read_le32(super + 24);
    fs->blocks_per_group = read_le32(super + 32);
    fs->inodes_per_group = read_le32(super + 40);
    revision = read_le32(super + 76);
    feature_incompat = read_le32(super + 96);

    if (log_block_size > 6) {
        fprintf(stderr, "Error: unsupported ext2 block-size exponent %u\n",
                log_block_size);
        goto fail;
    }
    fs->block_size = 1024U << log_block_size;
    fs->inode_size = revision == 0 ? EXT2_GOOD_OLD_INODE_SIZE
                                   : read_le16(super + 88);
    if (fs->inode_size == 0)
        fs->inode_size = EXT2_GOOD_OLD_INODE_SIZE;

    if (fs->inodes_count < EXT2_ROOT_INODE || fs->blocks_count == 0 ||
        fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
        fs->first_data_block >= fs->blocks_count ||
        fs->inode_size < EXT2_GOOD_OLD_INODE_SIZE ||
        fs->inode_size > fs->block_size || (fs->inode_size % 4) != 0) {
        fprintf(stderr, "Error: invalid ext2 superblock geometry\n");
        goto fail;
    }
    if ((feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) != 0) {
        fprintf(stderr, "Error: image uses unsupported incompatible filesystem features (0x%08x)\n",
                feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE);
        goto fail;
    }
    represented_size = (uint64_t)fs->blocks_count * fs->block_size;
    if (represented_size > (uint64_t)fs->image_size ||
        fs->inodes_count > (uint64_t)fs->image_size / EXT2_GOOD_OLD_INODE_SIZE + 1) {
        fprintf(stderr, "Error: ext2 geometry exceeds the image size\n");
        goto fail;
    }

    block_groups = ceil_div_u32(fs->blocks_count - fs->first_data_block,
                                fs->blocks_per_group);
    inode_groups = ceil_div_u32(fs->inodes_count, fs->inodes_per_group);
    fs->groups_count = block_groups > inode_groups ? block_groups : inode_groups;
    if (fs->groups_count == 0) {
        fprintf(stderr, "Error: invalid ext2 block-group count\n");
        goto fail;
    }
    fs->inode_tables = calloc(fs->groups_count, sizeof(*fs->inode_tables));
    if (fs->inode_tables == NULL) {
        fprintf(stderr, "Error: out of memory while loading group descriptors\n");
        goto fail;
    }

    descriptor_offset = (uint64_t)(fs->first_data_block + 1) * fs->block_size;
    for (group = 0; group < fs->groups_count; group++) {
        unsigned char descriptor[EXT2_GROUP_DESC_SIZE];
        uint64_t offset = descriptor_offset +
                          (uint64_t)group * EXT2_GROUP_DESC_SIZE;
        if (!image_read(fs, offset, descriptor, sizeof(descriptor))) {
            fprintf(stderr, "Error: cannot read ext2 group descriptor %u\n", group);
            goto fail;
        }
        fs->inode_tables[group] = read_le32(descriptor + 8);
        if (fs->inode_tables[group] == 0 ||
            fs->inode_tables[group] >= fs->blocks_count) {
            fprintf(stderr, "Error: invalid inode table in block group %u\n", group);
            goto fail;
        }
    }
    return 1;

fail:
    free(fs->inode_tables);
    fs->inode_tables = NULL;
    close(fs->fd);
    fs->fd = -1;
    return 0;
}

static void ext2_close(Ext2Fs *fs)
{
    free(fs->inode_tables);
    fs->inode_tables = NULL;
    if (fs->fd != -1)
        close(fs->fd);
    fs->fd = -1;
}

static int ext2_read_inode(const Ext2Fs *fs, uint32_t inode_number,
                           Ext2Inode *inode)
{
    unsigned char raw[EXT2_GOOD_OLD_INODE_SIZE];
    uint32_t group;
    uint32_t index;
    uint64_t offset;
    int i;

    if (inode_number == 0 || inode_number > fs->inodes_count)
        return 0;
    group = (inode_number - 1) / fs->inodes_per_group;
    index = (inode_number - 1) % fs->inodes_per_group;
    if (group >= fs->groups_count)
        return 0;
    offset = (uint64_t)fs->inode_tables[group] * fs->block_size +
             (uint64_t)index * fs->inode_size;
    if (!image_read(fs, offset, raw, sizeof(raw)))
        return 0;

    memset(inode, 0, sizeof(*inode));
    inode->mode = read_le16(raw + 0);
    inode->size_low = read_le32(raw + 4);
    inode->size_high = read_le32(raw + 108);
    for (i = 0; i < EXT2_N_BLOCKS; i++)
        inode->blocks[i] = read_le32(raw + 40 + i * 4);
    return 1;
}

static uint64_t ext2_inode_size(const Ext2Inode *inode)
{
    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
        return (uint64_t)inode->size_low | ((uint64_t)inode->size_high << 32);
    return inode->size_low;
}

static int ext2_read_pointer(const Ext2Fs *fs, uint32_t pointer_block,
                             uint64_t index, uint32_t *value)
{
    unsigned char raw[4];
    uint64_t pointers_per_block = fs->block_size / 4;
    uint64_t offset;

    if (pointer_block == 0) {
        *value = 0;
        return 1;
    }
    if (pointer_block >= fs->blocks_count || index >= pointers_per_block)
        return 0;
    offset = (uint64_t)pointer_block * fs->block_size + index * 4;
    if (!image_read(fs, offset, raw, sizeof(raw)))
        return 0;
    *value = read_le32(raw);
    if (*value >= fs->blocks_count)
        return 0;
    return 1;
}

/* Translate a logical file block through direct, single, double, and triple indirection. */
static int ext2_data_block(const Ext2Fs *fs, const Ext2Inode *inode,
                           uint64_t logical_block, uint32_t *physical_block)
{
    uint64_t per_block = fs->block_size / 4;
    uint64_t double_count = per_block * per_block;
    uint64_t triple_count;
    uint32_t first;
    uint32_t second;

    if (logical_block < EXT2_NDIR_BLOCKS) {
        *physical_block = inode->blocks[logical_block];
        return *physical_block < fs->blocks_count;
    }
    logical_block -= EXT2_NDIR_BLOCKS;
    if (logical_block < per_block)
        return ext2_read_pointer(fs, inode->blocks[12], logical_block,
                                 physical_block);

    logical_block -= per_block;
    if (logical_block < double_count) {
        if (!ext2_read_pointer(fs, inode->blocks[13], logical_block / per_block,
                               &first))
            return 0;
        return ext2_read_pointer(fs, first, logical_block % per_block,
                                 physical_block);
    }

    logical_block -= double_count;
    if (per_block > UINT64_MAX / double_count)
        return 0;
    triple_count = per_block * double_count;
    if (logical_block >= triple_count)
        return 0;
    if (!ext2_read_pointer(fs, inode->blocks[14], logical_block / double_count,
                           &first))
        return 0;
    logical_block %= double_count;
    if (!ext2_read_pointer(fs, first, logical_block / per_block, &second))
        return 0;
    return ext2_read_pointer(fs, second, logical_block % per_block,
                             physical_block);
}

static void entry_list_free(EntryList *list)
{
    EntryNode *node = list->head;
    while (node != NULL) {
        EntryNode *next = node->next;
        free(node);
        node = next;
    }
    list->head = NULL;
    list->count = 0;
}

/* Sorted insertion keeps output deterministic while satisfying the linked-list design. */
static int entry_list_insert(EntryList *list, uint32_t inode,
                             uint8_t file_type, const char *name)
{
    EntryNode **position = &list->head;
    EntryNode *node = malloc(sizeof(*node));
    if (node == NULL)
        return 0;
    node->inode = inode;
    node->file_type = file_type;
    strcpy(node->name, name);
    while (*position != NULL && strcmp((*position)->name, name) <= 0)
        position = &(*position)->next;
    node->next = *position;
    *position = node;
    list->count++;
    return 1;
}

/* Convert directory data blocks into a linked list of live entries. */
static int ext2_read_directory(const Ext2Fs *fs, uint32_t inode_number,
                               int hide_lost_found, EntryList *list)
{
    Ext2Inode inode;
    uint64_t directory_size;
    uint64_t block_count;
    uint64_t logical;
    unsigned char *block;

    memset(list, 0, sizeof(*list));
    if (!ext2_read_inode(fs, inode_number, &inode) ||
        (inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return 0;
    directory_size = ext2_inode_size(&inode);
    block_count = directory_size / fs->block_size +
                  (directory_size % fs->block_size != 0);
    block = malloc(fs->block_size);
    if (block == NULL)
        return 0;

    for (logical = 0; logical < block_count; logical++) {
        uint32_t physical;
        uint64_t consumed = logical * fs->block_size;
        size_t valid = (size_t)((directory_size - consumed) > fs->block_size
                                    ? fs->block_size
                                    : (directory_size - consumed));
        size_t offset = 0;

        if (!ext2_data_block(fs, &inode, logical, &physical))
            goto fail;
        if (physical == 0)
            continue;
        if (!image_read(fs, (uint64_t)physical * fs->block_size, block,
                        fs->block_size))
            goto fail;

        while (offset < valid) {
            uint32_t child_inode;
            uint16_t record_length;
            uint8_t name_length;
            uint8_t file_type;
            char name[EXT2_NAME_LEN + 1];

            if (valid - offset < 8)
                goto fail;
            child_inode = read_le32(block + offset);
            record_length = read_le16(block + offset + 4);
            name_length = block[offset + 6];
            file_type = block[offset + 7];
            if (record_length < 8 || (record_length % 4) != 0 ||
                record_length > valid - offset ||
                name_length > record_length - 8 || name_length > EXT2_NAME_LEN)
                goto fail;

            if (child_inode != 0) {
                if (child_inode > fs->inodes_count)
                    goto fail;
                memcpy(name, block + offset + 8, name_length);
                name[name_length] = '\0';
                if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                    (!hide_lost_found || strcmp(name, "lost+found") != 0)) {
                    if (!entry_list_insert(list, child_inode, file_type, name))
                        goto fail;
                }
            }
            offset += record_length;
        }
    }
    free(block);
    return 1;

fail:
    free(block);
    entry_list_free(list);
    return 0;
}

/* Paths are interpreted inside the image, always starting at inode 2. */
static int ext2_resolve_path(const Ext2Fs *fs, const char *path,
                             uint32_t *inode_number)
{
    char copy[PATH_MAX + 1];
    char *component;
    char *save = NULL;
    uint32_t current = EXT2_ROOT_INODE;

    if (path == NULL || *path == '\0' || path[0] == '/' ||
        strlen(path) > PATH_MAX)
        return 0;
    strcpy(copy, path);
    component = strtok_r(copy, "/", &save);
    while (component != NULL) {
        EntryList list;
        EntryNode *entry;
        int found = 0;
        if (strcmp(component, ".") == 0) {
            component = strtok_r(NULL, "/", &save);
            continue;
        }
        if (strcmp(component, "..") == 0)
            return 0;
        if (!ext2_read_directory(fs, current, 0, &list))
            return 0;
        for (entry = list.head; entry != NULL; entry = entry->next) {
            if (strcmp(entry->name, component) == 0) {
                current = entry->inode;
                found = 1;
                break;
            }
        }
        entry_list_free(&list);
        if (!found)
            return 0;
        component = strtok_r(NULL, "/", &save);
    }
    *inode_number = current;
    return 1;
}

static char ext2_type_character(uint16_t mode)
{
    switch (mode & EXT2_S_IFMT) {
    case EXT2_S_IFDIR: return 'd';
    case EXT2_S_IFREG: return '-';
    case EXT2_S_IFLNK: return 'l';
    case EXT2_S_IFCHR: return 'c';
    case EXT2_S_IFBLK: return 'b';
    case EXT2_S_IFIFO: return 'p';
    case EXT2_S_IFSOCK: return 's';
    default: return '?';
    }
}

static void ext2_permission_string(uint16_t mode, char output[11])
{
    static const uint16_t bits[9] = {
        0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001
    };
    static const char letters[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    int i;
    output[0] = ext2_type_character(mode);
    for (i = 0; i < 9; i++)
        output[i + 1] = (mode & bits[i]) ? letters[i] : '-';
    if (mode & 04000)
        output[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000)
        output[6] = (mode & 0010) ? 's' : 'S';
    if (mode & 01000)
        output[9] = (mode & 0001) ? 't' : 'T';
    output[10] = '\0';
}

static void print_entry_label(const char *name, const Ext2Inode *inode,
                              int show_size, int show_permissions)
{
    if (show_permissions) {
        char permissions[11];
        ext2_permission_string(inode->mode, permissions);
        if (show_size)
            printf("[%s %" PRIu64 "] %s", permissions,
                   ext2_inode_size(inode), name);
        else
            printf("[%s] %s", permissions, name);
    } else if (show_size) {
        printf("[%" PRIu64 "] %s", ext2_inode_size(inode), name);
    } else {
        fputs(name, stdout);
    }
}

static int visited_test(const unsigned char *visited, uint32_t inode)
{
    return (visited[inode / 8] & (unsigned char)(1U << (inode % 8))) != 0;
}

static void visited_set(unsigned char *visited, uint32_t inode)
{
    visited[inode / 8] |= (unsigned char)(1U << (inode % 8));
}

static int print_tree_children(const Ext2Fs *fs, uint32_t directory_inode,
                               const char *prefix, int recursive,
                               int show_size, int show_permissions,
                               unsigned char *visited, TreeCount *count,
                               unsigned depth)
{
    EntryList list;
    EntryNode *entry;

    if (!ext2_read_directory(fs, directory_inode, 1, &list))
        return 0;
    for (entry = list.head; entry != NULL; entry = entry->next) {
        Ext2Inode child;
        int is_last = entry->next == NULL;
        int is_directory;

        if (!ext2_read_inode(fs, entry->inode, &child)) {
            entry_list_free(&list);
            return 0;
        }
        is_directory = (child.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
        if (is_directory)
            count->directories++;
        else
            count->files++;

        printf("%s%s ", prefix, is_last ? "└──" : "├──");
        print_entry_label(entry->name, &child, show_size, show_permissions);
        putchar('\n');

        if (recursive && is_directory && depth < MAX_TREE_DEPTH &&
            !visited_test(visited, entry->inode)) {
            size_t length = strlen(prefix);
            char *next_prefix = malloc(length + 5);
            if (next_prefix == NULL) {
                entry_list_free(&list);
                return 0;
            }
            memcpy(next_prefix, prefix, length);
            strcpy(next_prefix + length, is_last ? "    " : "│   ");
            visited_set(visited, entry->inode);
            if (!print_tree_children(fs, entry->inode, next_prefix, recursive,
                                     show_size, show_permissions, visited,
                                     count, depth + 1)) {
                free(next_prefix);
                entry_list_free(&list);
                return 0;
            }
            free(next_prefix);
        }
    }
    entry_list_free(&list);
    return 1;
}

static const char *path_display_name(const char *path, char output[PATH_MAX + 1])
{
    size_t length;
    const char *slash;
    strcpy(output, path);
    length = strlen(output);
    while (length > 1 && output[length - 1] == '/')
        output[--length] = '\0';
    if (strcmp(output, ".") == 0 || strcmp(output, "./") == 0)
        return ".";
    slash = strrchr(output, '/');
    return slash == NULL ? output : slash + 1;
}

static void print_tree_usage(void)
{
    printf("Usage : tree <PATH> [OPTION]...\n");
}

static void print_file_usage(void)
{
    printf("Usage : print <PATH> [OPTION]...\n");
}

static void command_tree(const Ext2Fs *fs, char *tokens[], int token_count)
{
    const char *path;
    int recursive = 0;
    int show_size = 0;
    int show_permissions = 0;
    uint32_t inode_number;
    Ext2Inode inode;
    unsigned char *visited;
    size_t visited_size;
    TreeCount count = {1, 0};
    char display_buffer[PATH_MAX + 1];
    int i;

    if (token_count < 2) {
        print_tree_usage();
        return;
    }
    path = tokens[1];
    for (i = 2; i < token_count; i++) {
        size_t j;
        if (tokens[i][0] != '-' || tokens[i][1] == '\0') {
            print_tree_usage();
            return;
        }
        for (j = 1; tokens[i][j] != '\0'; j++) {
            if (tokens[i][j] == 'r') recursive = 1;
            else if (tokens[i][j] == 's') show_size = 1;
            else if (tokens[i][j] == 'p') show_permissions = 1;
            else {
                print_tree_usage();
                return;
            }
        }
    }
    if (!ext2_resolve_path(fs, path, &inode_number)) {
        print_tree_usage();
        return;
    }
    if (!ext2_read_inode(fs, inode_number, &inode)) {
        printf("Error: cannot read inode for '%s'\n", path);
        return;
    }
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        printf("Error: '%s' is not directory\n", path);
        return;
    }

    print_entry_label(path_display_name(path, display_buffer), &inode,
                      show_size, show_permissions);
    putchar('\n');
    visited_size = (size_t)fs->inodes_count / 8 + 1;
    visited = calloc(visited_size, 1);
    if (visited == NULL) {
        printf("Error: out of memory while building directory tree\n");
        return;
    }
    visited_set(visited, inode_number);
    if (!print_tree_children(fs, inode_number, "", recursive, show_size,
                             show_permissions, visited, &count, 0)) {
        printf("Error: malformed directory data in image\n");
        free(visited);
        return;
    }
    free(visited);
    printf("\n%" PRIu64 " directories, %" PRIu64 " files\n\n",
           count.directories, count.files);
}

static int parse_positive_line_count(const char *text, uint64_t *value)
{
    char *end;
    uintmax_t parsed;
    if (text == NULL || *text == '\0' || *text == '-')
        return 0;
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0 || parsed > UINT64_MAX)
        return 0;
    *value = (uint64_t)parsed;
    return 1;
}

static int print_inode_data(const Ext2Fs *fs, const Ext2Inode *inode,
                            int limited, uint64_t line_limit)
{
    uint64_t size = ext2_inode_size(inode);
    uint64_t blocks = size / fs->block_size + (size % fs->block_size != 0);
    uint64_t logical;
    uint64_t lines = 0;
    unsigned char *buffer = malloc(fs->block_size);
    if (buffer == NULL)
        return 0;

    for (logical = 0; logical < blocks; logical++) {
        uint32_t physical;
        uint64_t consumed = logical * fs->block_size;
        size_t amount = (size_t)((size - consumed) > fs->block_size
                                    ? fs->block_size : (size - consumed));
        size_t output_amount = amount;
        size_t i;

        if (!ext2_data_block(fs, inode, logical, &physical)) {
            free(buffer);
            return 0;
        }
        if (physical == 0)
            memset(buffer, 0, amount);
        else if (!image_read(fs, (uint64_t)physical * fs->block_size,
                             buffer, amount)) {
            free(buffer);
            return 0;
        }
        if (limited) {
            for (i = 0; i < amount; i++) {
                if (buffer[i] == '\n' && ++lines == line_limit) {
                    output_amount = i + 1;
                    if (fwrite(buffer, 1, output_amount, stdout) != output_amount) {
                        free(buffer);
                        return 0;
                    }
                    free(buffer);
                    return 1;
                }
            }
        }
        if (fwrite(buffer, 1, output_amount, stdout) != output_amount) {
            free(buffer);
            return 0;
        }
    }
    free(buffer);
    return 1;
}

static void command_print(const Ext2Fs *fs, char *tokens[], int token_count)
{
    const char *path;
    int limited = 0;
    uint64_t line_limit = 0;
    uint32_t inode_number;
    Ext2Inode inode;

    if (token_count < 2) {
        print_file_usage();
        return;
    }
    path = tokens[1];
    if (token_count > 2) {
        if (strcmp(tokens[2], "-n") != 0) {
            print_file_usage();
            return;
        }
        if (token_count == 3) {
            printf("print: option requires an argument -- 'n'\n");
            return;
        }
        if (token_count != 4 ||
            !parse_positive_line_count(tokens[3], &line_limit)) {
            print_file_usage();
            return;
        }
        limited = 1;
    }
    if (!ext2_resolve_path(fs, path, &inode_number)) {
        print_file_usage();
        return;
    }
    if (!ext2_read_inode(fs, inode_number, &inode)) {
        printf("Error: cannot read inode for '%s'\n", path);
        return;
    }
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
        printf("Error: '%s' is not file\n", path);
        return;
    }
    if (!print_inode_data(fs, &inode, limited, line_limit))
        printf("\nError: cannot read file data for '%s'\n", path);
}

static void print_tree_help(void)
{
    printf("Usage : tree <PATH> [OPTION]... : display the directory structure if <PATH> is a directory\n");
    printf("  -r : display the directory structure recursively\n");
    printf("  -s : include the size of each directory and file\n");
    printf("  -p : include the permissions of each directory and file\n");
}

static void print_print_help(void)
{
    printf("Usage : print <PATH> [OPTION]... : print file contents on standard output\n");
    printf("  -n <line_number> : print only the first <line_number> lines\n");
}

static void print_all_help(void)
{
    printf("Usage:\n");
    printf("  > tree <PATH> [OPTION]... : display the directory structure if <PATH> is a directory\n");
    printf("    -r : display the directory structure recursively\n");
    printf("    -s : include the size of each directory and file\n");
    printf("    -p : include the permissions of each directory and file\n");
    printf("  > print <PATH> [OPTION]... : print file contents on standard output\n");
    printf("    -n <line_number> : print only the first <line_number> lines\n");
    printf("  > help [COMMAND] : show commands for program\n");
    printf("  > exit : exit program\n");
}

static void command_help(char *tokens[], int token_count)
{
    if (token_count == 1) {
        print_all_help();
        return;
    }
    if (token_count == 2 && strcmp(tokens[1], "tree") == 0)
        print_tree_help();
    else if (token_count == 2 && strcmp(tokens[1], "print") == 0)
        print_print_help();
    else if (token_count == 2 && strcmp(tokens[1], "help") == 0)
        printf("Usage : help [COMMAND] : show commands for program\n");
    else if (token_count == 2 && strcmp(tokens[1], "exit") == 0)
        printf("Usage : exit : exit program\n");
    else {
        if (token_count == 2)
            printf("invalid command -- '%s'\n", tokens[1]);
        else
            printf("Usage : help [COMMAND]\n");
        print_all_help();
    }
}

/* Minimal quote/backslash handling allows ext2 names containing spaces. */
static int tokenize(char *line, char *tokens[], int maximum)
{
    char *readp = line;
    char *writep = line;
    int count = 0;
    while (*readp != '\0') {
        char quote = '\0';
        while (isspace((unsigned char)*readp))
            readp++;
        if (*readp == '\0')
            break;
        if (count == maximum)
            return -1;
        tokens[count++] = writep;
        while (*readp != '\0') {
            if (quote != '\0') {
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
            if (*readp == '\\' && readp[1] != '\0')
                readp++;
            *writep++ = *readp++;
        }
        if (quote != '\0')
            return -2;
        *writep++ = '\0';
    }
    return count;
}

static int execute_command(const Ext2Fs *fs, char *line)
{
    char *tokens[MAX_TOKENS];
    int count = tokenize(line, tokens, MAX_TOKENS);
    if (count == 0)
        return 1;
    if (count < 0) {
        print_all_help();
        return 1;
    }
    if (strcmp(tokens[0], "tree") == 0)
        command_tree(fs, tokens, count);
    else if (strcmp(tokens[0], "print") == 0)
        command_print(fs, tokens, count);
    else if (strcmp(tokens[0], "help") == 0)
        command_help(tokens, count);
    else if (strcmp(tokens[0], "exit") == 0) {
        if (count == 1)
            return 0;
        printf("Usage : exit\n");
    } else {
        print_all_help();
    }
    return 1;
}

static void prompt_loop(const Ext2Fs *fs)
{
    char command[COMMAND_MAX + 2];
    int running = 1;
    while (running) {
        size_t length;
        int character;
        printf(PROMPT);
        fflush(stdout);
        if (fgets(command, sizeof(command), stdin) == NULL)
            break;
        length = strlen(command);
        if (length > 0 && command[length - 1] != '\n' && !feof(stdin)) {
            while ((character = getchar()) != '\n' && character != EOF)
                ;
            print_all_help();
            continue;
        }
        command[strcspn(command, "\n")] = '\0';
        running = execute_command(fs, command);
    }
}

int main(int argc, char *argv[])
{
    Ext2Fs fs;
    if (argc != 2) {
        fprintf(stderr, "Usage Error : %s <EXT2_IMAGE>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!ext2_open(&fs, argv[1]))
        return EXIT_FAILURE;
    prompt_loop(&fs);
    ext2_close(&fs);
    return EXIT_SUCCESS;
}
