// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
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

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
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
            // Fast diff: check metadata instead of re-hashing file content
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
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
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
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
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

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    FILE *f;
    char line[1024];

    if (!index) return -1;
    index->count = 0;

    f = fopen(INDEX_FILE, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        IndexEntry *entry;
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime;
        unsigned int size;

        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        entry = &index->entries[index->count];
        if (sscanf(line, "%o %64s %llu %u %511[^\n]",
                   &entry->mode, hex, &mtime, &size, entry->path) != 5) {
            fclose(f);
            return -1;
        }

        if (hex_to_hash(hex, &entry->hash) != 0) {
            fclose(f);
            return -1;
        }

        entry->mtime_sec = (uint64_t)mtime;
        entry->size = size;
        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    FILE *f = NULL;
    Index *sorted = NULL;
    char temp_path[512];
    int rc = -1;

    if (!index) return -1;

    sorted = malloc(sizeof(Index));
    if (!sorted) return -1;

    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE);

    f = fopen(temp_path, "w");
    if (!f) goto cleanup;

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];

        hash_to_hex(&sorted->entries[i].hash, hex);
        if (fprintf(f, "%o %s %llu %u %s\n",
                    sorted->entries[i].mode,
                    hex,
                    (unsigned long long)sorted->entries[i].mtime_sec,
                    sorted->entries[i].size,
                    sorted->entries[i].path) < 0) {
            goto cleanup;
        }
    }

    if (fflush(f) != 0) goto cleanup;
    if (fsync(fileno(f)) != 0) goto cleanup;
    if (fclose(f) != 0) {
        f = NULL;
        goto cleanup;
    }
    f = NULL;

    if (rename(temp_path, INDEX_FILE) != 0) goto cleanup;

    rc = 0;

cleanup:
    if (f) fclose(f);
    if (rc != 0) unlink(temp_path);
    free(sorted);
    return rc;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    FILE *f = NULL;
    struct stat st;
    void *data = NULL;
    size_t read_size;
    ObjectID blob_id;
    IndexEntry *entry;
    int rc = -1;

    if (!index || !path) return -1;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    f = fopen(path, "rb");
    if (!f) return -1;

    data = malloc((size_t)st.st_size == 0 ? 1 : (size_t)st.st_size);
    if (!data) goto cleanup;

    read_size = (size_t)st.st_size;
    if (read_size > 0 && fread(data, 1, read_size, f) != read_size) goto cleanup;
    if (fclose(f) != 0) {
        f = NULL;
        goto cleanup;
    }
    f = NULL;

    if (object_write(OBJ_BLOB, data, read_size, &blob_id) != 0) goto cleanup;

    entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) goto cleanup;
        entry = &index->entries[index->count++];
    }

    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    if (snprintf(entry->path, sizeof(entry->path), "%s", path) >= (int)sizeof(entry->path)) goto cleanup;
    if (index_save(index) != 0) goto cleanup;

    rc = 0;

cleanup:
    free(data);
    if (f) fclose(f);
    return rc;
}

