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

static int write_tree_level(IndexEntry *entries, int count, const char *base_path, int base_len, ObjectID *id_out) {
    if (count == 0) return -1;
    
    Tree tree;
    tree.count = 0;
    
    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;
        const char *relative = path + base_len;
        
        
        if (*relative == '/') relative++;
        
        const char *next_slash = strchr(relative, '/');
        if (next_slash == NULL) {
            
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = entries[i].mode;
            memcpy(&entry->hash, &entries[i].hash, sizeof(ObjectID));
            strncpy(entry->name, relative, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            i++;
        } else {
            
            int name_len = next_slash - relative;
            char dir_name[256];
            strncpy(dir_name, relative, name_len);
            dir_name[name_len] = '\0';
            
            
            int subdir_count = 0;
            int start_idx = i;
            char subdir_path[512];
            snprintf(subdir_path, sizeof(subdir_path), "%s%.*s/", base_path, name_len, relative);
            int subdir_len = strlen(subdir_path);
            
            while (i < count) {
                const char *check_path = entries[i].path + base_len;
                if (*check_path == '/') check_path++;
                
                if (strncmp(check_path, dir_name, name_len) == 0 && 
                    (check_path[name_len] == '/' || check_path[name_len] == '\0')) {
                    subdir_count++;
                    i++;
                } else {
                    break;
                }
            }
            ObjectID subtree_id;
            if (write_tree_level(entries + start_idx, subdir_count, subdir_path, subdir_len, &subtree_id) != 0) {
                return -1;
            }
            
            
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = MODE_DIR;
            memcpy(&entry->hash, &subtree_id, sizeof(ObjectID));
            strncpy(entry->name, dir_name, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
        }
    }

    qsort(tree.entries, tree.count, sizeof(TreeEntry), compare_tree_entries);
    
    
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) {
        return -1;
    }
    
    int result = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    
    return result;
    return 0;
}
int tree_from_index(ObjectID *id_out) {
    IndexEntry *entries = NULL;
    int entry_count = index_load(&entries);
    if (entry_count < 0) return -1;
    
    if (entry_count == 0) {
        free(entries);
        return -1;
    }
    int result = write_tree_level(entries, entry_count, "", 0, id_out);
    
    free(entries);
    return result;
    return -1;
}