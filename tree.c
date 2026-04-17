// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "index.h"


// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

static int compare_tree_entries(const void *a, const void *b);

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.

// ─── TODO: Implement these ───────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    memset(index, 0, sizeof(Index));
    
    FILE *fp = fopen(".pes/index", "r");
    if (!fp) {
        return 0;
    }
    
    char line[1024];
    int line_num = 0;
    
    while (fgets(line, sizeof(line), fp) && index->count < MAX_INDEX_ENTRIES) {
        line_num++;
        
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        IndexEntry *entry = &index->entries[index->count];
        
        char hash_hex[HASH_HEX_SIZE + 1];
        int parsed = sscanf(line, "%u %64s %lu %u %511s", 
                           &entry->mode, 
                           hash_hex, 
                           &entry->mtime_sec, 
                           &entry->size, 
                           entry->path);
        
        if (parsed != 5) {
            fclose(fp);
            return -1;
        }
        
        if (hex_to_hash(hash_hex, &entry->hash) != 0) {
            fclose(fp);
            return -1;
        }
        
        index->count++;
    }
    
    fclose(fp);
    return 0;
}

int index_save(const Index *index) {
    mkdir(".pes", 0755);
    
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), ".pes/index.tmp");
    
    FILE *fp = fopen(temp_path, "w");
    if (!fp) {
        return -1;
    }
    
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);
    
    for (int i = 0; i < sorted_index.count; i++) {
        const IndexEntry *entry = &sorted_index.entries[i];
        
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&entry->hash, hash_hex);
        
        fprintf(fp, "%u %s %lu %u %s\n", 
                entry->mode, 
                hash_hex, 
                entry->mtime_sec, 
                entry->size, 
                entry->path);
    }
    
    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(fp);
    
    if (rename(temp_path, ".pes/index") != 0) {
        unlink(temp_path);
        return -1;
    }
    
    int dir_fd = open(".pes", O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
    
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }
    
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }
    
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    unsigned char *content = malloc(file_size);
    if (!content) {
        fclose(fp);
        return -1;
    }
    
    size_t bytes_read = fread(content, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_size) {
        free(content);
        return -1;
    }
    
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, content, file_size, &blob_id) != 0) {
        free(content);
        return -1;
    }
    
    free(content);
    
    uint32_t mode = MODE_FILE;
    if (st.st_mode & S_IXUSR) {
        mode = MODE_EXEC;
    }
    
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->mode = mode;
        memcpy(&existing->hash, &blob_id, sizeof(ObjectID));
        existing->mtime_sec = st.st_mtime;
        existing->size = st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        
        IndexEntry *entry = &index->entries[index->count++];
        entry->mode = mode;
        memcpy(&entry->hash, &blob_id, sizeof(ObjectID));
        entry->mtime_sec = st.st_mtime;
        entry->size = st.st_size;
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    }
    
    return index_save(index);
}