// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob"; break;
        case OBJ_TREE:   type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }
    
    char header[256];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0 || header_len >= (int)sizeof(header)) return -1;
    
    header[header_len] = '\0';
    header_len++;
    
    size_t full_size = header_len + len;
    unsigned char *full_object = malloc(full_size);
    if (!full_object) return -1;
    
    memcpy(full_object, header, header_len);
    memcpy(full_object + header_len, data, len);
    
    compute_hash(full_object, full_size, id_out);
    
    if (object_exists(id_out)) {
        free(full_object);
        return 0;
    }
    
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    
    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    
    if (mkdir(shard_dir, 0755) != 0 && errno != EEXIST) {
        free(full_object);
        return -1;
    }
    
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/.tmp_XXXXXX", shard_dir);
    
    int temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        free(full_object);
        return -1;
    }
    
    ssize_t written = write(temp_fd, full_object, full_size);
    if (written != (ssize_t)full_size) {
        close(temp_fd);
        unlink(temp_path);
        free(full_object);
        return -1;
    }
    
    if (fsync(temp_fd) != 0) {
        close(temp_fd);
        unlink(temp_path);
        free(full_object);
        return -1;
    }
    
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));
    
    if (rename(temp_path, final_path) != 0) {
        close(temp_fd);
        unlink(temp_path);
        free(full_object);
        return -1;
    }
    
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
    
    close(temp_fd);
    free(full_object);
    
    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));
    
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(fp);
        return -1;
    }
    
    unsigned char *file_buffer = malloc(file_size);
    if (!file_buffer) {
        fclose(fp);
        return -1;
    }
    
    size_t bytes_read = fread(file_buffer, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_size) {
        free(file_buffer);
        return -1;
    }
    
    unsigned char *null_pos = memchr(file_buffer, '\0', file_size);
    if (!null_pos) {
        free(file_buffer);
        return -1;
    }
    
    size_t header_len = (null_pos - file_buffer) + 1;
    
    char type_str[32];
    size_t data_size;
    
    if (sscanf((char *)file_buffer, "%31s %zu", type_str, &data_size) != 2) {
        free(file_buffer);
        return -1;
    }
    
    size_t actual_data_size = file_size - header_len;
    if (actual_data_size != data_size) {
        free(file_buffer);
        return -1;
    }
    
    ObjectID computed_id;
    compute_hash(file_buffer, file_size, &computed_id);
    
    if (memcmp(computed_id.hash, id->hash, HASH_SIZE) != 0) {
        free(file_buffer);
        return -1;
    }
    
    if (strcmp(type_str, "blob") == 0) {
        *type_out = OBJ_BLOB;
    } else if (strcmp(type_str, "tree") == 0) {
        *type_out = OBJ_TREE;
    } else if (strcmp(type_str, "commit") == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        free(file_buffer);
        return -1;
    }
    
    *len_out = data_size;
    *data_out = malloc(data_size);
    if (!*data_out) {
        free(file_buffer);
        return -1;
    }
    
    memcpy(*data_out, file_buffer + header_len, data_size);
    free(file_buffer);
    
    return 0;
}