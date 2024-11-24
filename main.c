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

static void error_dtor(Error *err) {
    if (err->msg != NULL) {
        free(err->msg);
        err->msg = NULL;
    }
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
 * @brief designed for iterations over BitmapData
 * @note this can be modelled for horizontal as well as vertical since we are
 * working with linear bitmap array (@see BitmapData) */
typedef struct BitmapDataIterator {
    Pixel       *current;
    const Pixel *end;
    size_t       offset;
} BitmapDataIterator;

/**
 * @return linear size of the bitmap data from given dimensions */
static inline size_t bmp_size_raw(BitmapSize dimension) {
    return (size_t)dimension.width * dimension.height;
}

/**
 * @brief constructor for bitmap data iterator
 * @note bitmap data iterator does NOT check whether the 'length' exceed the end
 * of the bitmap data! */
static inline BitmapDataIterator bmp_it_ctor(Pixel *begin, size_t length,
                                             size_t offset) {
    return (BitmapDataIterator){begin, begin + length, offset};
}

/**
 * @brief checks whether iterator will reach the end on next iteration
 * @return valid pointer when not, otherwise NULL */
static inline BitmapDataIterator *bmp_it_peek(BitmapDataIterator *it) {
    if (it->current + it->offset < it->end) {
        return it;
    }
    return NULL;
}

/**
 * @brief moves the iterator by its offset
 * @note stops at `it->end` if overflow occurs */
static inline BitmapDataIterator *bmp_it_next(BitmapDataIterator *it) {
    if (bmp_it_peek(it)) {
        it->current += it->offset;
        return it;
    }

    it->current = (void *)it->end;
    return NULL;
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
                          "Unexpected character encountered: '%c'", c);
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
 *                  Line
 * ========================================= */

typedef struct Point {
    uint32_t x, y;
} Point;

static inline Point point_invalid() { return (Point){UINT32_MAX, UINT32_MAX}; }

static inline bool point_is_invalid(Point p) {
    return p.x == UINT32_MAX && p.y == UINT32_MAX;
}

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
static inline uint32_t hline_length(HLine line) {
    return line.end.x - line.begin.x;
}
static inline uint32_t vline_length(VLine line) {
    return line.end.y - line.begin.y;
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

#define hline_cmp(lhs, rhs) (line_cmp((lhs), (rhs), hline_length))
#define vline_cmp(lhs, rhs) (line_cmp((lhs), (rhs), vline_length))

/**
 * @brief searches for a horizonal line from a given iterator */
static HLine scan_for_hline(BitmapDataIterator *hor_it, const uint32_t curr_row,
                            uint32_t *col_counter) {
    if (*(hor_it->current) == PXL_EMPTY) {
        return line_invalid();
    }

    Point start = {.x = *col_counter, .y = curr_row};
    Point end = start;

    bmp_it_next(hor_it);
    (*col_counter)++;
    for (; hor_it->current < hor_it->end && *(hor_it->current) == PXL_FILLED;
         (*col_counter)++) {
        bmp_it_next(hor_it);
        end.x++;
    }

    return line_ctor(start, end);
}

/**
 * @brief searches for a vertical line from a given iterator */
static VLine scan_for_vline(BitmapDataIterator *ver_it, const uint32_t curr_col,
                            uint32_t *row_counter) {
    if (*(ver_it->current) == PXL_EMPTY) {
        return line_invalid();
    }

    Point start = {.x = curr_col, .y = *row_counter};
    Point end = start;

    /* move to the next pxl, otherwise it is just a point */
    bmp_it_next(ver_it);
    (*row_counter)++;
    for (; ver_it->current < ver_it->end && *(ver_it->current) == PXL_FILLED;
         (*row_counter)++) {
        bmp_it_next(ver_it);
        end.y++;
    }

    return line_ctor(start, end);
}

/** @brief scans for longest horizontal line */
static HLine find_longest_hline(Bitmap *bmp) {
    HLine              max = line_invalid(), temp = {0};
    BitmapDataIterator hor_it = {0};
    /* scan each row */
    for (uint32_t row = 0; row < bmp->dimensions.height; row++) {
        /* create an horizontal iterator to move through the row */
        hor_it = bmp_it_ctor(bmp->data + row * bmp->dimensions.width,
                             bmp->dimensions.width, 1);
        /* find each line in a row and compare it to the max */
        for (uint32_t col = 0; col < bmp->dimensions.width;
             col++, bmp_it_next(&hor_it)) {
            temp = scan_for_hline(&hor_it, row, &col);
            if (line_is_invalid(temp)) {
                continue;
            }
            if (line_is_invalid(max)) {
                max = temp;
                continue;
            }
            if (hline_cmp(max, temp) < 0) {
                max = temp;
            }
        }
    }
    return max;
}

/** @brief scans for longest vertical line */
static VLine find_longest_vline(Bitmap *bmp) {
    VLine              max = line_invalid(), scanned = {0};
    BitmapDataIterator ver_it = {0};
    /* scan each column */
    for (uint32_t col = 0; col < bmp->dimensions.width; col++) {
        /* create an vertical iterator to move through the column */
        ver_it = bmp_it_ctor(
            bmp->data + col,
            (bmp->dimensions.height - 1) * bmp->dimensions.width + col,
            bmp->dimensions.width);
        /* find each line in a column and compare it to the max */
        for (uint32_t row = 0; row < bmp->dimensions.height;
             row++, bmp_it_next(&ver_it)) {
            scanned = scan_for_vline(&ver_it, col, &row);
            if (line_is_invalid(scanned)) {
                continue;
            }
            if (line_is_invalid(max)) {
                max = scanned;
                continue;
            }
            if (vline_cmp(max, scanned) < 0) {
                max = scanned;
            }
        }
    }
    return max;
}

/* =========================================
 *                  Square
 * ========================================= */

typedef struct Square {
    Point left_up, right_down;
} Square;

static inline Square square_ctor(Point left_up, Point right_down) {
    return (Square){left_up, right_down};
}

static inline Square square_invalid() {
    return square_ctor(point_invalid(), point_invalid());
}

static inline bool square_is_invalid(Square s) {
    return point_is_invalid(s.left_up) && point_is_invalid(s.right_down);
}

/**
 * @brief compares squares based on their side length
 * @note this function implicitly assumes that given arguments are squares
 * @return <0 when s1<s2; =0 when s1==s2; >0 when s1>s2; */
static int square_cmp(Square s1, Square s2) {
    uint32_t square_side_delta =
        (s1.right_down.y - s1.left_up.y) - (s2.right_down.y - s2.left_up.y);
    if (square_side_delta != 0) {
        return square_side_delta;
    }
    /* note: the logic here is inverted: the more closer it is to (0, 0); the
     * "bigger" the square */
    if (s2.left_up.y != s1.left_up.y) {
        return s2.left_up.y - s1.left_up.y;
    }
    return s2.left_up.x - s1.left_up.x;
}

static inline void square_print(Square s1) {
    printf("%" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", s1.left_up.y,
           s1.left_up.x, s1.right_down.y, s1.right_down.x);
}

/**
 * @note function assumes that the "start" point is of PXL_FILLED value! */
static Line move_along_orthogonal_square_sides(Point               start,
                                               BitmapDataIterator *hor_it,
                                               BitmapDataIterator *ver_it) {
    Line l = line_ctor(start, start);

    /* iterate while both iterators are valid and point to filled pixels */
    for (;;) {
        /* check horizontal iterator */
        if (bmp_it_peek(hor_it) == NULL ||
            *(hor_it->current + hor_it->offset) != PXL_FILLED) {
            break;
        }

        /* check vertical iterator */
        if (bmp_it_peek(ver_it) == NULL ||
            *(ver_it->current + ver_it->offset) != PXL_FILLED) {
            break;
        }

        /* advance both iterators */
        bmp_it_next(hor_it);
        bmp_it_next(ver_it);

        l.begin.x++;
        l.end.y++;
    }

    return l;
}

/**
 * @brief scans whether a given Point is a Square (1x1) or is a left anchor
 * (Square::left_up) of a larger square
 * @return false when no such square was found, true when square was found (its
 * memory is then written to "out_square") */
static Square scan_for_square(const Bitmap *bmp, Point left_up) {
    /* note: because of the possibility that the square has the minimum
     * size of 1x1, we cannot check for an anchor, just a pixel */
    if (bmp->data[left_up.y * bmp->dimensions.width + left_up.x] ==
        PXL_FILLED) {
        /* store the initial ver[tical] and hor[izontal] iterator */
        const char *begin =
            &bmp->data[left_up.y * bmp->dimensions.width + left_up.x];
        BitmapDataIterator hor_it =
            bmp_it_ctor((void *)begin, bmp->dimensions.width - left_up.x, 1);
        BitmapDataIterator ver_it = bmp_it_ctor(
            (void *)begin,
            (bmp->dimensions.height - left_up.y) * bmp->dimensions.width,
            bmp->dimensions.width);

        /* move the iterators horizontally and vertically, creating a
         * hypotenuse == diagonal of the (potential) square */
        Line diagonal =
            move_along_orthogonal_square_sides(left_up, &hor_it, &ver_it);

        /* if we should find a square, we can predict where the next
         * square point (Square::right_down) will be */
        Point expected_right_down = {
            .x = diagonal.begin.x,
            .y = diagonal.end.y,
        };

        /* check the same for the symmetric side (via "parallel" iterators) */
        BitmapDataIterator par_hor_it =
            bmp_it_ctor(ver_it.current, bmp->dimensions.width - left_up.x, 1);
        BitmapDataIterator par_ver_it = bmp_it_ctor(
            hor_it.current,
            (bmp->dimensions.height - left_up.y) * bmp->dimensions.width,
            bmp->dimensions.width);

        Point track_right_down = {.x = diagonal.end.x, .y = diagonal.begin.y};
        for (; bmp_it_peek(&par_hor_it) != NULL &&
               *par_hor_it.current == PXL_FILLED && bmp_it_peek(&par_ver_it) &&
               *par_ver_it.current == PXL_FILLED;) {
            /* note: the iterators could potentially overflow the square since
             * we cannot know whether or not the squares have distinct
             * sides/anchors */
            if (expected_right_down.x == track_right_down.x &&
                expected_right_down.y == track_right_down.y) {
                return square_ctor(left_up, expected_right_down);
            }
            track_right_down.x++;
            track_right_down.y++;
            bmp_it_next(&par_hor_it);
            bmp_it_next(&par_ver_it);
        }

        if (expected_right_down.x == track_right_down.x &&
            expected_right_down.y == track_right_down.y) {
            return square_ctor(left_up, expected_right_down);
        }
    }

    return square_invalid();
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
                                     Line (*line_search)(Bitmap *bmp)) {
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
    line_print(max_line);
    /* cleanup and return */
    bmp_dtor(&bmp);
    return error_none();
}

/** @brief scans for the biggest square */
static Error cmd_find_biggest_square(const char *file_name) {
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
    Square max = square_invalid(), scanned = {0};
    for (uint32_t row = 0; row < bmp.dimensions.height; row++) {
        /* for every pixel in the row, check whether this pixel extends
         * to orthogonal sides of a square (or itself in case of 1x1) */
        for (uint32_t col = 0; col < bmp.dimensions.width; col++) {
            scanned = scan_for_square(&bmp, (Point){col, row});
            if (square_is_invalid(scanned)) {
                continue;
            }
            if (square_is_invalid(max)) {
                max = scanned;
                continue;
            }
            /* if any square was found, compare to the currrent maximum */
            if (square_cmp(max, scanned) < 0) {
                max = scanned;
            }
        }
    }
    /* print results */
    square_print(max);
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
            return cmd_execute_search_line(cmd->file_name, find_longest_hline);
        case VLINE:
            return cmd_execute_search_line(cmd->file_name, find_longest_vline);
        case SQUARE:
            return cmd_find_biggest_square(cmd->file_name);
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
            error_dtor(&err);
            return err.code;
        }
    }

    /* execute given command */
    {
        Error err = cmd_execute(&cmd);
        if (err.code != ERR_NONE) {
            error_print(err);
            error_dtor(&err);
            return err.code;
        }
    }

    return EXIT_SUCCESS;
}
