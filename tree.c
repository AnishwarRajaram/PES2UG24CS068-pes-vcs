#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR 0040000

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int write_tree_level(IndexEntry *entries, int count, const char *base_path, int base_len, ObjectID *id_out) {
    if (count == 0) return -1;
    
    Tree tree;
    tree.count = 0;
    
    int i = 0;
    while (i < count && tree.count < MAX_TREE_ENTRIES) {
        const char *path = entries[i].path;
        const char *relative = path + base_len;
        
        if (*relative == '/') relative++;
        
        const char *next_slash = strchr(relative, '/');
        
        if (next_slash == NULL) {
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = entries[i].mode;
            memcpy(&entry->hash, &entries[i].hash, sizeof(ObjectID));
            size_t name_len = strlen(relative);
            if (name_len >= sizeof(entry->name)) name_len = sizeof(entry->name) - 1;
            memcpy(entry->name, relative, name_len);
            entry->name[name_len] = '\0';
            i++;
        } else {
            int name_len = next_slash - relative;
            char dir_name[256];
            if (name_len >= (int)sizeof(dir_name)) name_len = sizeof(dir_name) - 1;
            memcpy(dir_name, relative, name_len);
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
            size_t dir_name_len = strlen(dir_name);
            if (dir_name_len >= sizeof(entry->name)) dir_name_len = sizeof(entry->name) - 1;
            memcpy(entry->name, dir_name, dir_name_len);
            entry->name[dir_name_len] = '\0';
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
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    memset(&index, 0, sizeof(Index));
    
    if (index_load(&index) != 0) {
        return -1;
    }
    
    if (index.count == 0) {
        return -1;
    }
    
    int result = write_tree_level(index.entries, index.count, "", 0, id_out);
    
    return result;
}