#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PXIR_U8,
    PXIR_U16,
    PXIR_U32,
    PXIR_U64
} pxir_integer_type;

static int parse_type(const char *name, pxir_integer_type *out) {
    if (strcmp(name, "u8") == 0) { *out = PXIR_U8; return 1; }
    if (strcmp(name, "u16") == 0) { *out = PXIR_U16; return 1; }
    if (strcmp(name, "u32") == 0) { *out = PXIR_U32; return 1; }
    if (strcmp(name, "u64") == 0) { *out = PXIR_U64; return 1; }
    return 0;
}

static const char *type_name(pxir_integer_type type) {
    switch (type) {
        case PXIR_U8: return "uint8_t";
        case PXIR_U16: return "uint16_t";
        case PXIR_U32: return "uint32_t";
        case PXIR_U64: return "uint64_t";
    }
    return "unknown";
}

static size_t type_size(pxir_integer_type type) {
    switch (type) {
        case PXIR_U8: return sizeof(uint8_t);
        case PXIR_U16: return sizeof(uint16_t);
        case PXIR_U32: return sizeof(uint32_t);
        case PXIR_U64: return sizeof(uint64_t);
    }
    return 0;
}

static uint64_t touch_and_checksum(void *memory, pxir_integer_type type, size_t count) {
    uint64_t checksum = 0;

    switch (type) {
        case PXIR_U8: {
            volatile uint8_t *values = memory;
            for (size_t i = 0; i < count; ++i) values[i] = (uint8_t)(i % 251u);
            for (size_t i = 0; i < count; ++i) checksum += values[i];
            break;
        }
        case PXIR_U16: {
            volatile uint16_t *values = memory;
            for (size_t i = 0; i < count; ++i) values[i] = (uint16_t)(i % 251u);
            for (size_t i = 0; i < count; ++i) checksum += values[i];
            break;
        }
        case PXIR_U32: {
            volatile uint32_t *values = memory;
            for (size_t i = 0; i < count; ++i) values[i] = (uint32_t)(i % 251u);
            for (size_t i = 0; i < count; ++i) checksum += values[i];
            break;
        }
        case PXIR_U64: {
            volatile uint64_t *values = memory;
            for (size_t i = 0; i < count; ++i) values[i] = (uint64_t)(i % 251u);
            for (size_t i = 0; i < count; ++i) checksum += values[i];
            break;
        }
    }

    return checksum;
}

static int parse_count(const char *text, size_t *out) {
    if (text == NULL || *text == '\0') return 0;

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return 0;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || parsed == 0) return 0;
    if (parsed > (unsigned long long)SIZE_MAX) return 0;

    *out = (size_t)parsed;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <u8|u16|u32|u64> <count>\n", argv[0]);
        return 2;
    }

    pxir_integer_type type;
    size_t count;

    if (!parse_type(argv[1], &type)) {
        fprintf(stderr, "error: unsupported type '%s'\n", argv[1]);
        return 2;
    }

    if (!parse_count(argv[2], &count)) {
        fprintf(stderr, "error: count must contain only digits and be greater than zero\n");
        return 2;
    }

    const size_t bytes_per_element = type_size(type);
    if (bytes_per_element == 0 || count > SIZE_MAX / bytes_per_element) {
        fprintf(stderr, "error: requested allocation size overflows size_t\n");
        return 2;
    }

    const size_t requested_bytes = count * bytes_per_element;
    void *memory = malloc(requested_bytes);
    if (memory == NULL) {
        fprintf(stderr, "error: allocation of %zu bytes failed\n", requested_bytes);
        return 3;
    }

    const uint64_t checksum = touch_and_checksum(memory, type, count);

    printf("pxir_experiment=001\n");
    printf("type=%s\n", type_name(type));
    printf("count=%zu\n", count);
    printf("bytes_per_element=%zu\n", bytes_per_element);
    printf("requested_bytes=%zu\n", requested_bytes);
    printf("value_pattern=i_mod_251\n");
    printf("checksum=%" PRIu64 "\n", checksum);

    free(memory);
    return 0;
}
