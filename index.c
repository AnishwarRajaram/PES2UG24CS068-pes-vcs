// index.c — Staging area implementation

#include "index.h"
#include "object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

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
    while (fgets(line, sizeof(line), fp) && index->count < MAX_INDEX_ENTRIES) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        IndexEntry *entry = &index->entries[index->count];
        char hash_hex[HASH_HEX_SIZE + 1];
        
        if (sscanf(line, "%u %64s %lu %u %511s", 
                   &entry->mode, 
                   hash_hex, 
                   &entry->mtime_sec, 
                   &entry->size, 
                   entry->path) != 5) {
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