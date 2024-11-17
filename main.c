#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================
 *                  Error
 * ========================================= */

typedef int ErrorNum;
#define ERR_NONE (ErrorNum)0
#define ERR_INTERNAL (ErrorNum)0x000000B5
#define ERR_INVALID_NUMBER_ARGS (ErrorNum)0xB16B00B5
#define ERR_INVALID_COMMAND (ErrorNum)0xBAADF00D
#define ERR_INVALID_BITMAP_FILE (ErrorNum)0xDEADBAAD
#define ERR_READ_RAW_BITMAP (ErrorNum)0xBAD8EAD
#define ERR_BITMAP_TEST (ErrorNum)0x8BADF00D
#define ERR_INVALID_DIMENSION (ErrorNum)0xABADBABE

typedef struct Error {
    ErrorNum code;
    char *msg;
} Error;

Error error_ctor(ErrorNum err, const char *fmt, ...) {
    /* construct the formatted message of the error */
    va_list fmt_args;
    va_start(fmt_args, fmt);

    /* required err msg size */
    int size = vsnprintf(NULL, 0, fmt, fmt_args) + 1;

    char *err_msg = calloc(size, sizeof(char));
    if (!err_msg) {
        fprintf(stderr,
                "Internal error: allocation failed while creating error "
                "object. Requested size: %d bytes.\n (Error code: %#x)",
                size, ERR_INTERNAL);
        return (Error){ERR_INTERNAL, NULL};
    }

    /* write formatted message */
    size = vsnprintf(err_msg, size, fmt, fmt_args);
    va_end(fmt_args);

    return (Error){err, err_msg};
}

static inline Error error_none(void) { return (Error){ERR_NONE, NULL}; }

static inline void error_print(Error err) { fprintf(stderr, "%s\n", err.msg); }

void error_dtor(Error *err) {
    if (err->msg) {
        free(err->msg);
        err->msg = NULL;
        err->code = ERR_NONE;
    }
}

/* =========================================
 *                  Bitmap
 * ========================================= */

typedef struct BitmapSize {
    uint32_t width;
    uint32_t height;
} BitmapSize;

typedef char Pixel;
typedef Pixel *BitmapData;

typedef struct Bitmap {
    BitmapSize dimensions;
    BitmapData data;
} Bitmap;

/**
 * @brief BitmapLoader is responsible for loading bitmap
 */
typedef struct BitmapLoader {
    /** @brief staging bitmap buffer */
    Bitmap staging;

    /* BitmapLoader metadata */

    /** @brief stores the filename of the bitmap file */
    const char *file_name;
    /** @brief holds the current number of pixels stored in bitmap */
    size_t size;
} BitmapLoader;

typedef struct BitmapIterator {
    char *begin;
    char *end;
    size_t offset;
} BitmapIterator;

static inline size_t bmp_size_raw(BitmapSize dimension) {
    return (size_t)dimension.width * dimension.height;
}

BitmapIterator bmp_it_ctor(char *begin, char *end, ptrdiff_t offset) {
    return (BitmapIterator){begin, end, offset};
}

BitmapIterator *bmp_it_move(BitmapIterator *it) {
    if (it->begin + it->offset < it->end) {
        it->begin += it->offset;
    } else {
        // note: is it fine to "damp" the iterator or should we throw ???
        it->begin = it->end;
    }
    return it;
}

Bitmap bmp_ctor(BitmapSize dimensions) {
    /* TODO !!!!!!!! */
    /*
    if (raw.staging.data == NULL) {
        fprintf(stderr,
                "Memory allocation failed for BitmapLoader of size %d x %d\n",
                sz.width, sz.height);
    }*/
    return (Bitmap){
        .dimensions = dimensions,
        .data = malloc(sizeof(Pixel) * bmp_size_raw(dimensions) + 1),
    };
}

void bmp_dtor(Bitmap *bmp) {
    if (bmp->data) {
        free(bmp->data);
        bmp->dimensions = (BitmapSize){0};
    }
}

BitmapLoader bmp_loader_ctor(const char *file_name) {
    return (BitmapLoader){
        .staging.dimensions = (BitmapSize){0},
        .staging.data = NULL,

        .size = 0,
        .file_name = file_name,
    };
}

void bmp_loader_dtor(BitmapLoader *loader) {
    if (loader->staging.data) {
        free(loader->staging.data);
        loader->size = 0;
        loader->file_name = NULL;
    }
}

/** @note there is only issue with "bmp_loader_dtor" being accidently called
 * after this call .... */
static inline Bitmap bmp_loader_get_bitmap(BitmapLoader *loader) {
    /* copy the final bitmap before invalidation */
    Bitmap final = loader->staging;

    /* invalidate loader's staging buffer */
    loader->staging = (Bitmap){0};

    return final;
}

BitmapLoader *bmp_loader_add_pixel(BitmapLoader *loader, Pixel c) {
    if (loader->size >= bmp_size_raw(loader->staging.dimensions)) {
        return NULL;
    }
    loader->staging.data[loader->size++] = c;
    return loader;
}

static inline bool bmp_valid_whitespace(char c) {
    return c == ' ' || c == '\n';
}
static inline bool bmp_valid_pix(Pixel c) { return c == '1' || c == '0'; }

Error bmp_loader_ignore_whitespace(FILE *file, BitmapLoader *loader) {
    int c = fgetc(file);
    for (; c != EOF; c = fgetc(file)) {
        if (bmp_valid_whitespace(c)) {
            continue;
        }
        if (bmp_valid_pix(c)) {
            if (!bmp_loader_add_pixel(loader, c)) {
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

static inline Error bmp_loader_load_dimension(FILE *file, uint32_t *dimension) {
    /* TODO !!!!!!!! */
    int32_t ret = fscanf(file, "%u", dimension);
    if (ret != 1) {
        return error_ctor(ERR_INVALID_DIMENSION,
                          "Given dimension [%d], is not an int value!",
                          *dimension);
    }
    return error_none();
}

static inline Error bmp_loader_load_size(FILE *file, BitmapSize *out_size) {
    Error err = bmp_loader_load_dimension(file, &out_size->height);
    /* TODO !!!!!!!!!! */
    if (err.code) {
        return err;
    }
    return bmp_loader_load_dimension(file, &out_size->width);
}

/**
 * @brief loads bitmap into loader's staging buffer
 */
Error bmp_loader_load(BitmapLoader *loader) {
    /* try to open the file */
    FILE *file = fopen(loader->file_name, "r");
    /* return on error */
    if (file == NULL) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "Given bitmap file [%s] does not exist!\n",
                          loader->file_name);
    }
    /* if success, load */

    /* load the data size from file */
    BitmapSize size = {0};
    Error err = bmp_loader_load_size(file, &size);
    if (err.code) {
        fclose(file);
        return err;
    }

    /* allocate staging buffer */
    loader->staging = bmp_ctor(size);

    /* filter whitespace from file into the raw bitmap */
    err = bmp_loader_ignore_whitespace(file, loader);
    if (err.code) {
        fclose(file);
        return err;
    }

    /* free resources upon function leave */
    fclose(file);

    return error_none();
}

/* =========================================
 *                  Line
 * ========================================= */

typedef struct Point {
    uint32_t x, y;
} Point;

typedef struct Line {
    Point begin, end;
} Line;
typedef Line VLine;
typedef Line HLine;

static inline Line line_ctor(Point begin, Point end) {
    return (Line){begin, end};
}

static inline void line_print(Line line) {
    printf("%d %d %d %d\n", line.begin.y, line.begin.x, line.end.y, line.end.x);
}

static inline uint32_t hline_length(HLine line) {
    return line.end.x - line.begin.x;
}

static inline uint32_t vline_length(VLine line) {
    return line.end.y - line.begin.y;
}

HLine scan_for_hline_internal(BitmapIterator *it, const uint32_t curr_row,
                              uint32_t *col_counter) {
    Point start = {.x = *col_counter, .y = curr_row};
    Point end = start;

    for (; it->begin < it->end && *(it->begin) == '1'; (*col_counter)++) {
        bmp_it_move(it);
        end.x++;
    }

    return line_ctor(start, end);
}

/**
 * @brief scans given row of bitmap for the longest horizontal line
 * @param out_max is set iff its initial value is lower than the max row line
 */
void scan_for_hline(BitmapIterator it, const uint32_t curr_row,
                    HLine *out_max) {
    uint32_t col_counter = 0;
    for (; it.begin < it.end; bmp_it_move(&it), col_counter++) {
        HLine max = scan_for_hline_internal(&it, curr_row, &col_counter);
        if (hline_length(*out_max) < hline_length(max)) {
            *out_max = max;
        }
    }
}

VLine scan_for_vline_internal(BitmapIterator *it, const uint32_t curr_col,
                              uint32_t *row_counter) {
    Point start = {.x = curr_col, .y = *row_counter};
    Point end = start;

    (*row_counter)++;
    for (; it->begin < it->end && *(it->begin) == '1'; (*row_counter)++) {
        bmp_it_move(it);
        end.y++;
    }

    return line_ctor(start, end);
}

void scan_for_vline(BitmapIterator it, const uint32_t curr_col,
                    VLine *out_max) {
    uint32_t row_counter = 0;
    for (; it.begin < it.end; bmp_it_move(&it)) {
        VLine max = scan_for_vline_internal(&it, curr_col, &row_counter);
        if (vline_length(*out_max) < vline_length(max)) {
            *out_max = max;
        }
    }
}

/* =========================================
 *                 Command
 * ========================================= */

typedef enum CommandType { HELP = 0, TEST, HLINE, VLINE, SQUARE } CommandType;

typedef struct Command {
    CommandType type;
    const char *file_name;
} Command;

/**
 * @brief executes "--help" figsearch command by printing basic data about the
 * command options */
Error command_help(void) {
    printf(
        "Figsearch algorithm\n"
        "-------------------\n"
        "usage: figsearch [command] (optional)[bitmap location]\n"
        "possible commands:\n"
        "\t\t--help\n"
        "\t\ttest\n"
        "\t\thline\n"
        "\t\tvline\n"
        "\t\tsquare\n");
    return error_none();
}

/**
 * @brief executes "test" figsearch command by trying to load the bitmap file
 * @return ERR_INVALID_BITMAP_FILE with message "Invalid"
 * @see bmp_load when bmp raw param is set to NULL
 */
static inline Error command_test(const char *file_name) {
    /* TODO !!!! */
    BitmapLoader temp = bmp_loader_ctor(file_name);

    Error err = bmp_loader_load(&temp);
    {
        if (err.code) {
            error_dtor(&err);
            bmp_loader_dtor(&temp);
            return error_ctor(ERR_INVALID_BITMAP_FILE, "Invalid");
        }

        printf("Valid\n");
    }

    bmp_loader_dtor(&temp);

    return error_none();
}

Error command_line(const char *file_name, Line (*line_search)(Bitmap *bmp)) {
    /* load bitmap */
    Bitmap bmp = {0};
    {
        BitmapLoader loader = bmp_loader_ctor(file_name);
        Error err = bmp_loader_load(&loader);
        if (err.code) {
            bmp_loader_dtor(&loader);
            return err;
        }
        bmp = bmp_loader_get_bitmap(&loader);
    }

    /* scan for longest line */
    Line max_line = line_search(&bmp);

    /* print results */
    line_print(max_line);

    /* cleanup */
    bmp_dtor(&bmp);

    return error_none();
}

/** @brief scans for longest horizontal line */
HLine command_hline_search(Bitmap *bmp) {
    // todo: will there always be horizontal line ???
    HLine max = {0};
    for (uint32_t i = 0; i < bmp->dimensions.height; i++) {
        scan_for_hline(bmp_it_ctor(
                           /* begin of i-th row */
                           (i * bmp->dimensions.width) + bmp->data,
                           /* end of i-th row */
                           ((i + 1) * bmp->dimensions.width) + bmp->data,
                           /* offset by */
                           sizeof(Pixel)),
                       i, &max);
    }
    return max;
}

/** @brief scans for longest vertical line */
VLine command_vline_search(Bitmap *bmp) {
    // todo: will there always be vertical line ???
    VLine max = {0};
    for (uint32_t i = 0; i < bmp->dimensions.width; i++) {
        scan_for_vline(bmp_it_ctor(
                           /* i-th column */
                           i + bmp->data,
                           /* i-th column offsetted by row - 1 */
                           (i + bmp->data) + (bmp->dimensions.height - 1) *
                                                 bmp->dimensions.width,
                           /* number of iterations = number of rows */
                           bmp->dimensions.width),
                       i, &max);
    }
    return max;
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
            return command_line(cmd->file_name, command_hline_search);
        case VLINE:
            return command_line(cmd->file_name, command_vline_search);
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

    /* TODO !!!!! */
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
        if (err.code) {
            error_print(err);
            error_dtor(&err);
            return err.code;
        }
        error_dtor(&err);
    }

    /* execute given command */
    {
        Error err = command_execute(&cmd);
        if (err.code) {
            error_print(err);
            error_dtor(&err);
            return err.code;
        }
        error_dtor(&err);
    }

    printf("exiting...");

    return ERR_NONE;
}
