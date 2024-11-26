#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================
 *                Constants
 * ========================================= */

#define ERR_NONE                (0)
#define ERR_ALLOCATION_FAILURE  (0x00FA75E5)
#define ERR_INTERNAL            (0x000000B5)
#define ERR_INVALID_NUMBER_ARGS (0xB16B00B5)
#define ERR_INVALID_COMMAND     (0xBAADF00D)
#define ERR_INVALID_BITMAP_FILE (0xDEADBAAD)
#define ERR_READ_RAW_BITMAP     (0x0BAD8EAD)
#define ERR_BITMAP_TEST         (0x8BADF00D)
#define ERR_INVALID_DIMENSION   (0xABADBABE)

#define PXL_FILLED ('1')
#define PXL_EMPTY  ('0')

#define CMD_MAX_ARGS (3)
#define CMD_MIN_ARGS (2)

#define COORD_INVALID (UINT32_MAX)

#define BMP_LOADER_MAX_PXL_CHUNK_SIZE (256)

/* =========================================
 *                  Error
 * ========================================= */

typedef int ErrorNum;
typedef struct Error {
    ErrorNum code;
    char    *msg;
} Error;

static Error error_ctor(ErrorNum err, const char *fmt, ...) {
    /* construct the formatted message of the error */
    va_list fmt_args;
    va_start(fmt_args, fmt);

    /* required err msg size */
    int size = vsnprintf(NULL, 0, fmt, fmt_args) + 1;

    char *err_msg = calloc(size, sizeof(char));
    if (err_msg == NULL) {
        /* note: since we cannot allocate, we cannot really create the message
         * and pass it through the call stack */
        fprintf(stderr,
                "Internal error: allocation failed while creating error "
                "object. Requested size: %d bytes.\n (Error code: %#x)",
                size, ERR_ALLOCATION_FAILURE);
        return (Error){ERR_ALLOCATION_FAILURE, NULL};
    }

    /* write formatted message */
    size = vsnprintf(err_msg, size, fmt, fmt_args);
    va_end(fmt_args);

    return (Error){err, err_msg};
}

static inline Error error_none(void) { return (Error){ERR_NONE, NULL}; }

static inline void error_print(Error err) { fprintf(stderr, "%s\n", err.msg); }

/**
 * @brief destroys the error's allocated memory
 * @return the last error of `err` param */
static ErrorNum error_dtor(Error *err) {
    ErrorNum err_code = ERR_NONE;
    if (err->msg != NULL) {
        err_code = err->code;
        free(err->msg);
        err->msg = NULL;
        err->code = ERR_NONE;
    }
    return err_code;
}

/* =========================================
 *                  Bitmap
 * ========================================= */

typedef struct BitmapSize {
    uint32_t width;
    uint32_t height;
} BitmapSize;

typedef char   Pixel;
typedef Pixel *BitmapData;

typedef struct Bitmap {
    BitmapSize dimensions;
    BitmapData data;
} Bitmap;

typedef struct BitmapLoader {
    /** @brief staging bitmap buffer */
    Bitmap staging;

    /* BitmapLoader metadata */

    /** @brief stores the filename of the bitmap file */
    const char *file_name;
    /** @brief holds the current number of pixels stored in staging buffer */
    size_t size;
} BitmapLoader;

/**
 * @return linear size of the bitmap data from given dimensions */
static inline size_t bmp_size_raw(BitmapSize dimension) {
    return (size_t)dimension.width * dimension.height;
}

/**
 * @brief creates bitmap based on the bitmap size
 * @return ERR_NONE when out_bmp was successfully populated */
static Error bmp_ctor(BitmapSize dimensions, Bitmap *out_bmp) {
    out_bmp->dimensions = dimensions;
    out_bmp->data = malloc(sizeof(Pixel) * bmp_size_raw(dimensions) + 1);
    if (out_bmp->data == NULL) {
        return error_ctor(ERR_ALLOCATION_FAILURE,
                          "Failed to allocate bitmap data buffer!\n");
    }
    return error_none();
}

static Pixel *bmp_at(const Bitmap *bmp, uint32_t row, uint32_t col) {
    /* todo: consider maybe dividing non-check into macro and checked here */
    return &bmp->data[row * bmp->dimensions.width + col];
}

static void bmp_dtor(Bitmap *bmp) {
    if (bmp->data != NULL) {
        free(bmp->data);
        bmp->dimensions = (BitmapSize){0};
    }
}

/** @brief creates default bitmap loader */
static inline BitmapLoader bmp_loader_ctor(const char *file_name) {
    return (BitmapLoader){
        .staging.dimensions = (BitmapSize){0},
        .staging.data = NULL,
        .size = 0,
        .file_name = file_name,
    };
}

/** @brief destroys bitmap loader */
static void bmp_loader_dtor(BitmapLoader *restrict loader) {
    if (loader->staging.data != NULL) {
        free(loader->staging.data);
        loader->size = 0;
        loader->file_name = NULL;
    }
}

/**
 * @brief returns bmp loader's staging buffer to a bitmap
 * @note the function does not check whether a given bitmap load was successfull
 * @note bitmap loader's buffer is invalidated, cannot be retrieved again */
static inline Bitmap bmp_loader_get_bitmap(BitmapLoader *restrict loader) {
    /* copy the final bitmap before invalidation */
    Bitmap final = loader->staging;
    /* invalidate loader's staging buffer */
    loader->staging = (Bitmap){0};
    return final;
}

/**
 * @brief adds pixel to the bmp loader's staging buffer
 * @return NULL when loader's memory is exceeded */
static BitmapLoader *bmp_loader_add_pixel(BitmapLoader *restrict loader,
                                          Pixel c) {
    if (loader->size >= bmp_size_raw(loader->staging.dimensions)) {
        return NULL;
    }
    loader->staging.data[loader->size++] = c;
    return loader;
}

static inline bool bmp_valid_whitespace(char c) { return isspace(c); }
static inline bool bmp_valid_pix(Pixel c) {
    return c == PXL_FILLED || c == PXL_EMPTY;
}

/**
 * @brief populates loader's staging buffer with the file data
 * @note function assumes that the file pointer is pointing to a pixel value
 * @return error when staging buffer oveflowed or when an invalid character
 * encountered */
static Error bmp_loader_ignore_whitespace(FILE *file,
                                          BitmapLoader *restrict loader) {
    /* buffered reading */
    while (!feof(file)) {
        char   buffer[BMP_LOADER_MAX_PXL_CHUNK_SIZE] = {0};
        size_t read =
            fread(&buffer, sizeof(char), sizeof(buffer) / sizeof(char), file);
        for (size_t i = 0; i < read; i++) {
            if (bmp_valid_whitespace(buffer[i])) {
                continue;
            }
            if (bmp_valid_pix(buffer[i])) {
                if (!bmp_loader_add_pixel(loader, buffer[i])) {
                    return error_ctor(ERR_INVALID_BITMAP_FILE,
                                      "the raw bitmap size does not match given"
                                      "dimensions!");
                }
                continue;
            }
            return error_ctor(ERR_INVALID_BITMAP_FILE,
                              "unexpected character encountered: '%c'",
                              buffer[i]);
        }
    }
    return error_none();
}

/**
 * @brief loads bitmap dimension into out_dimension
 * @return error_none when loaded successfully, otherwsise Error::code > 0 */
static inline Error bmp_loader_load_dimension(FILE     *file,
                                              uint32_t *out_dimension) {
    int ret = fscanf(file, "%" PRIu32, out_dimension);
    if (ret != 1) {
        return error_ctor(ERR_INVALID_DIMENSION,
                          "Given dimension [%" PRIu32 "], is not an int value!",
                          *out_dimension);
    }
    return error_none();
}

/**
 * @brief loads bitmap dimensions
 * @return error_none when loaded successfully, otherwise Error::code > 0 */
static inline Error bmp_loader_load_size(FILE *file, BitmapSize *out_size) {
    Error err = bmp_loader_load_dimension(file, &out_size->height);
    if (err.code != ERR_NONE) {
        return err;
    }
    return bmp_loader_load_dimension(file, &out_size->width);
}

/**
 * @brief loads a bitmap into the staging buffer
 * @note ensure the loader has a unique staging buffer to avoid violations
 * @return `error_none` on success; otherwise, an error with `Error::code > 0`
 */
static Error bmp_loader_load(BitmapLoader *restrict loader) {
    /* try to open bitmap */
    FILE *file = fopen(loader->file_name, "r");
    if (file == NULL) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "Failed to open file [%s]! Os error: \n",
                          loader->file_name, strerror(errno));
    }
    /* load the data size from file */
    BitmapSize size = {0};
    Error      err = bmp_loader_load_size(file, &size);
    if (err.code != ERR_NONE) {
        fclose(file);
        return err;
    }
    /* allocate (blank) staging buffer for bitmap */
    err = bmp_ctor(size, &loader->staging);
    if (err.code != ERR_NONE) {
        fclose(file);
        return err;
    }
    /* filter whitespace from file into the staging buffer */
    err = bmp_loader_ignore_whitespace(file, loader);
    if (err.code != ERR_NONE) {
        fclose(file);
        return err;
    }
    /* assure that the bitmap file has been read correctly */
    size_t expected_size = bmp_size_raw(loader->staging.dimensions);
    if (loader->size != expected_size) {
        fclose(file);
        return error_ctor(ERR_INVALID_DIMENSION,
                          "The number of pixels found (%zu) is not the same as "
                          "defined in the header size (%" PRIu32 "x%" PRIu32
                          "=%" PRIu32 ")\n",
                          loader->size, loader->staging.dimensions.height,
                          loader->staging.dimensions.width,
                          bmp_size_raw(loader->staging.dimensions));
    }
    /* cleanup and return */
    fclose(file);
    return error_none();
}

/* =========================================
 *                 Point
 * ========================================= */

typedef struct Point {
    uint32_t x, y;
} Point;

static inline Point point_invalid() {
    return (Point){COORD_INVALID, COORD_INVALID};
}

static inline bool point_is_invalid(Point p) {
    return p.x == COORD_INVALID && p.y == COORD_INVALID;
}

/* =========================================
 *                  Line
 * ========================================= */

typedef struct Line {
    Point begin, end;
} Line;
typedef Line VLine;
typedef Line HLine;

static inline Line line_ctor(Point begin, Point end) {
    return (Line){begin, end};
}
static inline Line line_invalid() {
    return line_ctor(point_invalid(), point_invalid());
}
static inline bool line_is_invalid(Line l) {
    return point_is_invalid(l.begin) && point_is_invalid(l.end);
}
static inline void line_print(Line line) {
    printf("%d %d %d %d\n", line.begin.y, line.begin.x, line.end.y, line.end.x);
}
/**
 * @brief compares two lines based on their length
 * @return <0...lhs<rhs; >0...lhs>rhs; =0...lhs==rhs */
static int line_cmp(Line lhs, Line rhs, uint32_t (*length_func)(Line line)) {
    uint32_t length_delta = length_func(lhs) - length_func(rhs);
    if (length_delta != 0) {
        return length_delta;
    }
    if (lhs.begin.y != rhs.begin.y) {
        return rhs.begin.y - lhs.begin.y;
    }
    return rhs.begin.x - lhs.begin.x;
}
static inline uint32_t hline_length(HLine line) {
    return line.end.x - line.begin.x + 1;
}
static inline uint32_t vline_length(VLine line) {
    return line.end.y - line.begin.y + 1;
}

#define hline_cmp(lhs, rhs) (line_cmp((lhs), (rhs), hline_length))
#define vline_cmp(lhs, rhs) (line_cmp((lhs), (rhs), vline_length))

static HLine line_find_hline(const Bitmap *bmp, Point begin) {
    if (*bmp_at(bmp, begin.y, begin.x) == PXL_FILLED) {
        Point end = begin;
        end.x++;
        for (; end.x < bmp->dimensions.width &&
               *bmp_at(bmp, end.y, end.x) == PXL_FILLED;
             end.x++)
            ;
        end.x--;
        return line_ctor(begin, end);
    }
    return line_invalid();
}

static VLine line_find_vline(const Bitmap *bmp, Point begin) {
    if (*bmp_at(bmp, begin.y, begin.x) == PXL_FILLED) {
        Point end = {begin.x, begin.y};
        end.y++;
        for (; end.y < bmp->dimensions.height &&
               *bmp_at(bmp, end.y, end.x) == PXL_FILLED;
             end.y++) {
        }
        end.y--;
        return line_ctor(begin, end);
    }
    return line_invalid();
}

/** @brief scans for longest horizontal line */
static HLine line_find_longest_hline(const Bitmap *bmp) {
    HLine max = line_invalid(), temp = {0};
    for (uint32_t row = 0; row < bmp->dimensions.height; row++) {
        for (uint32_t col = 0; col < bmp->dimensions.width; col++) {
            temp = line_find_hline(bmp, (Point){col, row});
            if (line_is_invalid(temp)) {
                continue;
            }
            col = temp.end.x;
            if (line_is_invalid(max) || hline_cmp(max, temp) < 0) {
                max = temp;
            }
        }
    }
    return max;
}

/** @brief scans for longest vertical line */
static VLine line_find_longest_vline(const Bitmap *bmp) {
    VLine max = line_invalid(), temp = {0};
    for (uint32_t col = 0; col < bmp->dimensions.width; col++) {
        for (uint32_t row = 0; row < bmp->dimensions.height; row++) {
            if (bmp->data[row * bmp->dimensions.width + col] == PXL_FILLED) {
                uint32_t prev_row = row;
                row++;
                for (;
                     row < bmp->dimensions.height &&
                     bmp->data[row * bmp->dimensions.width + col] == PXL_FILLED;
                     row++)
                    ;
                temp = line_ctor((Point){.x = col, .y = prev_row},
                                 (Point){.x = col, .y = row - 1});
                if (line_is_invalid(max) || vline_cmp(max, temp) < 0) {
                    max = temp;
                }
            }
        }
    }
    return max;
}

/* =========================================
 *                  equare
 * ========================================= */

typedef struct Square {
    Point top_left, bottom_right;
} Square;

static inline Square square_ctor(Point top_left, Point bottom_right) {
    return (Square){top_left, bottom_right};
}
static inline Square square_invalid() {
    return square_ctor(point_invalid(), point_invalid());
}
static inline bool square_is_invalid(Square s) {
    return point_is_invalid(s.top_left) && point_is_invalid(s.bottom_right);
}

/**
 * @brief compares squares based on their side length
 * @note this function implicitly assumes that given arguments are squares
 * @return <0 when s1<s2; =0 when s1==s2; >0 when s1>s2; */
static int square_cmp(Square s1, Square s2) {
    uint32_t square_side_delta = (s1.bottom_right.y - s1.top_left.y) -
                                 (s2.bottom_right.y - s2.top_left.y);
    if (square_side_delta != 0) {
        return square_side_delta;
    }
    /* note: the logic here is inverted: the more closer it is to (0, 0); the
     * "bigger" the square */
    if (s2.top_left.y != s1.top_left.y) {
        return s2.top_left.y - s1.top_left.y;
    }
    return s2.top_left.x - s1.top_left.x;
}

static inline void square_print(Square s1) {
    printf("%" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", s1.top_left.y,
           s1.top_left.x, s1.bottom_right.y, s1.bottom_right.x);
}

static VLine square_find_vertical_side(const Bitmap *bmp, Point ver_begin) {
    return line_find_vline(bmp, ver_begin);
}

static VLine square_find_horizontal_side(const Bitmap *bmp, Point hor_begin) {
    return line_find_hline(bmp, hor_begin);
}

static Square square_find_largest_square(const Bitmap *bmp) {
    Square max = square_invalid(), temp = {0};
    for (uint32_t row = 0; row < bmp->dimensions.height; row++) {
        /* for every pixel in the row, check whether this pixel extends
         * to orthogonal sides of a square (or itself in case of 1x1) */
        for (uint32_t col = 0; col < bmp->dimensions.width; col++) {
            if (*bmp_at(bmp, row, col) == PXL_EMPTY) {
                continue;
            }
            Point    top_left = {col, row};
            uint32_t x_track = col, y_track = row;
            /* search for the largest orthogonal anchor */
            for (;;) {
                if (x_track > bmp->dimensions.width ||
                    *bmp_at(bmp, row, x_track) == PXL_EMPTY) {
                    break;
                }

                if (y_track > bmp->dimensions.height ||
                    *bmp_at(bmp, y_track, col) == PXL_EMPTY) {
                    break;
                }

                x_track++;
                y_track++;
            }
            x_track -= 1;
            y_track -= 1;
            /*  */
            Point expected_bottom_right = {x_track, y_track};
            HLine hline =
                square_find_horizontal_side(bmp, (Point){col, y_track});
            VLine vline = square_find_vertical_side(bmp, (Point){x_track, row});
            if (hline.end.x >= expected_bottom_right.x &&
                vline.end.y >= expected_bottom_right.y) {
                /* found square */
                temp = square_ctor(top_left, expected_bottom_right);
                if (square_is_invalid(max) || square_cmp(max, temp) < 0) {
                    max = temp;
                }
                /* note: we do not move the col offset because there can be
                 * other "square sides" inside the square we have found */
                continue;
            }
            /*
             * we have to search for the square "wrapped inside" the orthogonal
             * lines note: we can decrement x_track and y_track simultaneously
             * since they are offsetted by the same value from the top_left
             */
            for (; x_track >= col; x_track--, y_track--) {
                Point expected_bottom_right = {x_track, y_track};
                HLine hline =
                    square_find_horizontal_side(bmp, (Point){col, y_track});
                VLine vline =
                    square_find_vertical_side(bmp, (Point){x_track, row});
                if (hline.end.x >= expected_bottom_right.x &&
                    vline.end.y >= expected_bottom_right.y) {
                    /* found square */
                    temp = square_ctor(top_left, expected_bottom_right);
                    if (square_is_invalid(max) || square_cmp(max, temp) < 0) {
                        max = temp;
                    }
                    break;
                }
            }
        }
    }
    return max;
}

/* =========================================
 *                 Command
 * ========================================= */

typedef enum UserCommandAction {
    HELP = 0,
    TEST,
    HLINE,
    VLINE,
    SQUARE
} UserCommandAction;

typedef struct UserCommand {
    UserCommandAction action_type;
    const char       *file_name;
} UserCommand;

static const char *HELP_MESSAGE =
    "Figsearch Algorithm\n"
    "===================\n"
    "A tool to analyze bitmap images for specific geometric patterns.\n\n"
    "USAGE:\n"
    "    figsearch [command] [bitmap location]\n\n"
    "COMMANDS:\n"
    "    --help       Displays this help message.\n"
    "    test         Validates the specified bitmap file.\n"
    "                 Requires: [bitmap location].\n"
    "    hline        Finds the longest horizontal line in the bitmap.\n"
    "                 Requires: [bitmap location].\n"
    "    vline        Finds the longest vertical line in the bitmap.\n"
    "                 Requires: [bitmap location].\n"
    "    square       Detects the largest square in the bitmap.\n"
    "                 Requires: [bitmap location].\n\n"
    "NOTES:\n"
    "    - All commands (except --help) require the [bitmap location] "
    "argument.\n"
    "    - hline, vline and square commands implicitly check the validity of "
    "the file.\n"
    "    - The bitmap location should be a valid path to a bitmap file.\n"
    "    - Example usage: figsearch hline my_image.bmp\n";

/**
 * @brief executes "--help" figsearch command by printing basic data about the
 * command options */
static inline Error cmd_display_help_message(void) {
    printf("%s", HELP_MESSAGE);
    return error_none();
}

/**
 * @brief executes "test" figsearch command by trying to load the bitmap file
 * @return ERR_INVALID_BITMAP_FILE with message "Invalid" */
static inline Error cmd_validate_bitmap_file(const char *file_name) {
    BitmapLoader temp = bmp_loader_ctor(file_name);

    Error err = bmp_loader_load(&temp);
    {
        if (err.code != ERR_NONE) {
            error_dtor(&err);
            bmp_loader_dtor(&temp);
            return error_ctor(ERR_INVALID_BITMAP_FILE, "%s", "Invalid");
        }

        printf("Valid\n");
    }

    bmp_loader_dtor(&temp);

    return error_none();
}

static Error cmd_execute_search_line(const char *file_name,
                                     Line (*line_search)(const Bitmap *bmp)) {
    /* load bitmap */
    Bitmap bmp = {0};
    {
        BitmapLoader loader = bmp_loader_ctor(file_name);
        Error        err = bmp_loader_load(&loader);
        if (err.code != ERR_NONE) {
            bmp_loader_dtor(&loader);
            return err;
        }
        bmp = bmp_loader_get_bitmap(&loader);
    }
    /* scan for longest line */
    Line max_line = line_search(&bmp);
    /* print results */
    if (line_is_invalid(max_line)) {
        printf("Not found\n");
    } else {
        line_print(max_line);
    }
    /* cleanup and return */
    bmp_dtor(&bmp);
    return error_none();
}

/** @brief scans for the biggest square */
static Error cmd_execute_search_square(const char *file_name) {
    /* load bitmap */
    Bitmap bmp = {0};
    {
        BitmapLoader loader = bmp_loader_ctor(file_name);
        Error        err = bmp_loader_load(&loader);
        if (err.code != ERR_NONE) {
            bmp_loader_dtor(&loader);
            return err;
        }
        bmp = bmp_loader_get_bitmap(&loader);
    }
    /* search for biggest square */
    Square max = square_find_largest_square(&bmp);
    /* print results */
    if (square_is_invalid(max)) {
        printf("Not found\n");
    } else {
        square_print(max);
    }
    /* cleanup and return */
    bmp_dtor(&bmp);
    return error_none();
}

static Error cmd_execute(UserCommand *cmd) {
    switch (cmd->action_type) {
        case HELP:
            return cmd_display_help_message();
        case TEST:
            return cmd_validate_bitmap_file(cmd->file_name);
        case HLINE:
            return cmd_execute_search_line(cmd->file_name,
                                           line_find_longest_hline);
        case VLINE:
            return cmd_execute_search_line(cmd->file_name,
                                           line_find_longest_vline);
        case SQUARE:
            return cmd_execute_search_square(cmd->file_name);
    }
    return error_ctor(ERR_INTERNAL, "Invalid control path executed on line: %d",
                      __LINE__);
}

static Error cmd_parse(int argc, char **argv, UserCommand *out_cmd) {
    /* ensure that the command is of correct size */
    if (argc > CMD_MAX_ARGS || argc < CMD_MIN_ARGS) {
        return error_ctor(ERR_INVALID_NUMBER_ARGS,
                          "Invalid number of arguments given! Expected: "
                          "1 or 2 but given: %d.\nFor more info refer to "
                          "the help info:\n%s",
                          argc - 1, HELP_MESSAGE);
    }

    /* validate command args */
    if (argc == CMD_MIN_ARGS) /* HELP command */ {
        if (strcmp(argv[1], "--help") == 0) {
            out_cmd->action_type = HELP;
            return error_none();
        }
        return error_ctor(ERR_INVALID_COMMAND,
                          "Invalid command given [%s]! Expected: --help. Did "
                          "you forget to add bitmap file name?",
                          argv[1]);
    }

    /*
     * convenient macro for cmd member data assingment (command parse
     * function-only)
     */
#define register_command(reg_type, reg_file_name)               \
    (struct UserCommand) {                                      \
        .action_type = (reg_type), .file_name = (reg_file_name) \
    }

    if (strcmp(argv[1], "test") == 0) {
        *out_cmd = register_command(TEST, argv[2]);
        return error_none();
    }
    if (strcmp(argv[1], "hline") == 0) {
        *out_cmd = register_command(HLINE, argv[2]);
        return error_none();
    }
    if (strcmp(argv[1], "vline") == 0) {
        *out_cmd = register_command(VLINE, argv[2]);
        return error_none();
    }
    if (strcmp(argv[1], "square") == 0) {
        *out_cmd = register_command(SQUARE, argv[2]);
        return error_none();
    }

#undef register_command

    return error_ctor(ERR_INVALID_COMMAND,
                      "Invalid command given [%s]! Expected one of: --help, "
                      "test, hline, vline, sqaure.",
                      argv[1]);
}

int main(int argc, char **argv) {
    /* parse command line */
    UserCommand cmd = {0};
    {
        Error err = cmd_parse(argc, argv, &cmd);
        if (err.code != ERR_NONE) {
            error_print(err);
            return error_dtor(&err);
        }
    }

    /* execute given command */
    {
        Error err = cmd_execute(&cmd);
        if (err.code != ERR_NONE) {
            error_print(err);
            return error_dtor(&err);
        }
    }

    return EXIT_SUCCESS;
}
