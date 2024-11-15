#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t ErrorNum;
#define ERR_NONE (ErrorNum)0
#define ERR_INTERNAL (ErrorNum)0x000000B5
#define ERR_INVALID_NUMBER_ARGS (ErrorNum)0xB16B00B5
#define ERR_INVALID_COMMAND (ErrorNum)0xBAADF00D
#define ERR_INVALID_BITMAP_FILE (ErrorNum)0xDEADBAAD
#define ERR_READ_RAW_BITMAP (ErrorNum)0xBAD8EAD
#define ERR_BITMAP_TEST (ErrorNum)0x8BADF00D
#define ERR_INVALID_DIMENSION (ErrorNum)0xABADBABE

typedef struct Error {
    ErrorNum err;
    char *msg;
} Error;

Error error_ctor(ErrorNum err, const char *fmt, ...) {
    va_list fmt_args;
    va_start(fmt_args, fmt);
    int size = vsnprintf(NULL, 0, fmt, fmt_args) + 1;
    // todo: check allocation
    char *msg = calloc(size, sizeof(char));
    // todo: check printf
    size = vsnprintf(msg, size, fmt, fmt_args);
    va_end(fmt_args);

    return (Error){err, msg};
}

static inline Error error_none() { return (Error){ERR_NONE, NULL}; }

static inline void error_dispatch(Error err) { fprintf(stderr, "%s", err.msg); }

void error_dtor(Error err) {
    if (err.msg) {
        free(err.msg);
    }
}

typedef struct BitmapSize {
    uint32_t width;
    uint32_t height;
} BitmapSize;

typedef char Pixel;
typedef Pixel *BitmapData;

typedef struct BitmapRaw {
    BitmapSize dimensions;
    BitmapData data;
    size_t size;
    size_t cap;
} BitmapRaw;

typedef struct Bitmap {
    size_t size;
    BitmapData data;
} Bitmap;

/**
 * @brief constructs bitmap from raw bitmap
 * @note constructor invalidates raw bitmap data pointer
 */
Bitmap bmp_ctor(BitmapRaw *bmp_raw) {
    Bitmap bmp = (Bitmap){.data = bmp_raw->data, .size = bmp_raw->size};
    bmp_raw->data = NULL;
    return bmp;
}

void bmp_dtor(Bitmap *bmp) {
    if (bmp->data) {
        free(bmp->data);
    }
}

BitmapRaw bmp_raw_ctor(BitmapSize sz) {
    BitmapRaw raw = {0};
    raw.dimensions = sz;
    raw.size = 0;
    raw.cap = sz.height * sz.width;
    raw.data = malloc(raw.cap);
}

BitmapRaw *bmp_raw_add(BitmapRaw *bmp, Pixel c) {
    if (bmp->size + 1 >= bmp->cap) {
        return NULL;
    }
    bmp->data[bmp->size++] = c;
    return bmp;
}

void bmp_raw_dtor(BitmapRaw *bmp) {
    free(bmp->data);
    bmp->size = 0;
    bmp->cap = 0;
}

static inline bool valid_whitespace(char c) { return c == ' ' || c == '\n'; }
static inline bool valid_pix(char c) { return c == '1' || c == '0'; }

Error bmp_ignore_whitespace(FILE *file, BitmapRaw *out_raw) {
    int c = fgetc(file);
    for (; c != EOF; c = fgetc(file)) {
        if (valid_whitespace(c)) {
            continue;
        }
        if (valid_pix(c)) {
            if (!bmp_raw_add(out_raw, c)) {
                printf("Invalid!");
                return error_ctor(
                    ERR_INVALID_BITMAP_FILE,
                    "The raw bitmap size does not match given dimensions!");
            }
            continue;
        }
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "Unexpected character encountered: '%c'", (char)c);
    }
    return error_none();
}

static inline Error bmp_load_dimension(FILE *file, uint32_t *dimension) {
    int ret = fscanf(file, "%u", dimension);
    if (ret != 1) {
        return error_ctor(ERR_INVALID_DIMENSION,
                          "Given dimension [%d], is not an int value!",
                          dimension);
    }
    return error_none();
}

static inline Error bmp_load_size(FILE *file, BitmapSize *out_size) {
    Error err = bmp_load_dimension(file, &out_size->height);
    if (err.err) {
        return err;
    }
    return bmp_load_dimension(file, &out_size->width);
}

Error bmp_load_internal(const char *file_name, BitmapRaw *out_bmp) {
    /* try to open the file */
    FILE *file = fopen(file_name, "r");
    /* return on error */
    if (file == NULL) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "Given bitmap file [%s] does not exist!\n",
                          file_name);
    }
    /* if success, load raw */

    /* load the data size from the raw buffer */
    BitmapSize size = {0};
    Error err = bmp_load_size(file, &size);
    if (err.err) {
        fclose(file);
        return err;
    }

    /* allocate raw bitmap */
    *out_bmp = bmp_raw_ctor(size);

    /* filter whitespace from file into the raw bitmap */
    bmp_ignore_whitespace(file, out_bmp);

    /* free resources upon function leave */
    fclose(file);

    return error_none();
}

/**
 * @brief loads raw bitmap into "out_bmp"
 * @note if "out_bmp" param is NULL, function only validates file
 */
Error bmp_load(const char *file_name, BitmapRaw *out_bmp) {
    if (out_bmp) {
        return bmp_load_internal(file_name, out_bmp);
    }

    /* validate file */
    BitmapRaw bmp = {0};
    Error err = bmp_load_internal(file_name, &bmp);
    {
        if (err.err) {
            error_dtor(err);
            return error_ctor(ERR_INVALID_BITMAP_FILE, "Invalid");
        }
        error_dtor(err);
        return error_ctor(ERR_NONE, "Valid");
    }
}

typedef enum CommandType { HELP = 0, TEST, HLINE, VLINE, SQUARE } CommandType;

typedef struct Command {
    CommandType type;
    const char *file_name;
} Command;

Error command_help(void) {
    printf("Figsearch algorithm\n");
    printf("-------------------\n");
    printf("usage: figsearch [command] (optional)[bitmap location]\n");
    printf("possible commands:\n");
    printf("\t\t--help\n");
    printf("\t\ttest\n");
    printf("\t\thline\n");
    printf("\t\tvline\n");
    printf("\t\tsquare\n");
    return error_none();
}
Error command_test(const char *file_name) { return bmp_load(file_name, NULL); }
Error command_hline(const char *file_name) {
    printf("Hello from command_hline");
    Bitmap bmp = {0};
    {
        BitmapRaw raw = {0};
        Error err = bmp_load(file_name, &raw);
        if (err.err) {
            return err;
        }
        bmp = bmp_ctor(&raw);
    }
    assert(false);  // TODO
    return error_none();
}
Error command_vline(const char *file_name) {
    printf("Hello from command_vline");
    assert(false);  // TODO
    return error_none();
}
Error command_square(const char *file_name) {
    printf("Hello from command_square");
    assert(false);  // TODO
    return error_none();
}

Error command_execute(Command *cmd) {
    switch (cmd->type) {
        case HELP:
            return command_help();
        case TEST:
            return command_test(cmd->file_name);
        case HLINE:
            return command_hline(cmd->file_name);
        case VLINE:
            return command_vline(cmd->file_name);
        case SQUARE:
            return command_square(cmd->file_name);
    }
    return error_ctor(ERR_INTERNAL, "Invalid control path executed on line: %d",
                      __LINE__);
}

Error command_parse(int argc, char **argv, Command *out_cmd) {
    /* ensure that the command is of correct size */
    if (argc > 3 || argc < 2) {
        return error_ctor(
            ERR_INVALID_NUMBER_ARGS,
            "Invalid number of arguments given! Expected: "
            "1=[--help] or 2 but given: %d. See \"%s --help\" for more info\n",
            argc - 1, argv[0]);
    }

    /* validate command args */
    if (argc == 2) /* HELP command */ {
        if (strcmp(argv[1], "--help") == 0) {
            out_cmd->type = HELP;
            return error_none();
        }
        return error_ctor(ERR_INVALID_COMMAND,
                          "Invalid command given [%s]! Expected: --help. Did "
                          "you forget to add bitmap file name?",
                          argv[1]);
    }

/* convenient macro for cmd member data assingment (command parse function-only)
 */
#define register_cmd(cmd, cmd_type, cmd_file_name) \
    {                                              \
        (cmd).type = cmd_type;                     \
        (cmd).file_name = cmd_file_name;           \
    }

    if (strcmp(argv[1], "test") == 0) {
        register_cmd(*out_cmd, TEST, argv[2]);
        return error_none();
    }
    if (strcmp(argv[1], "hline") == 0) {
        register_cmd(*out_cmd, HLINE, argv[2]);
        return error_none();
    }
    if (strcmp(argv[1], "vline") == 0) {
        register_cmd(*out_cmd, VLINE, argv[2]);
        return error_none();
    }
    if (strcmp(argv[1], "square") == 0) {
        register_cmd(*out_cmd, SQUARE, argv[2]);
        return error_none();
    }

#undef register_cmd

    return error_ctor(ERR_INVALID_COMMAND,
                      "Invalid command given [%s]! Expected one of: --help, "
                      "test, hline, vline, sqaure.",
                      argv[1]);
}

int main(int argc, char **argv) {
    /* parse command line */
    Command cmd = {0};
    {
        Error err = command_parse(argc, argv, &cmd);
        if (err.err) {
            error_dispatch(err);
            error_dtor(err);
            return err.err;
        }
        error_dtor(err);
    }

    /* execute given command */
    {
        Error err = command_execute(&cmd);
        if (err.err) {
            error_dispatch(err);
            error_dtor(err);
            return err.err;
        }
        error_dtor(err);
    }

    printf("exiting...");

    return ERR_NONE;
}
