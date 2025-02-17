#include "diagnostic.h"
#include <locale.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include "preproc/lexer.h"

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define GET_SOURCE_LOC(loc) (&tokens->locations[loc])

static const char* report_names[] = {
    "verbose",
    "info",
    "warning",
    "error",
};

mtx_t report_mutex;

#if _WIN32
static HANDLE console_handle;
static WORD default_attribs;

const static int attribs[] = {
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    FOREGROUND_RED | FOREGROUND_INTENSITY,
};
#else
static const char* const attribs[] = {
    "\x1b[0m",
    "\x1b[32m",
    "\x1b[31m",
    "\x1b[31m",
};
#endif

bool report_using_thin_errors = false;

#if _WIN32
#define RESET_COLOR     SetConsoleTextAttribute(console_handle, default_attribs)
#define SET_COLOR_RED   SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | FOREGROUND_RED)
#define SET_COLOR_GREEN SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | FOREGROUND_GREEN)
#define SET_COLOR_WHITE SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#else
#define RESET_COLOR     printf("\x1b[0m")
#define SET_COLOR_RED   printf("\x1b[31m")
#define SET_COLOR_GREEN printf("\x1b[32m")
#define SET_COLOR_WHITE printf("\x1b[37m")
#endif

void init_report_system(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    #if _WIN32
    if (console_handle == NULL) {
        console_handle = GetStdHandle(STD_OUTPUT_HANDLE);

        // Enable ANSI/VT sequences on windows
        HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output_handle != INVALID_HANDLE_VALUE) {
            DWORD old_mode;
            if (GetConsoleMode(output_handle, &old_mode)) {
                SetConsoleMode(output_handle, old_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }

        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(console_handle, &info);

        default_attribs = info.wAttributes;
    }

    #endif

    mtx_init(&report_mutex, mtx_plain | mtx_recursive);
}

static void print_level_name(Cuik_ReportLevel level) {
    #if _WIN32
    SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | attribs[level]);
    printf("%s: ", report_names[level]);
    SetConsoleTextAttribute(console_handle, default_attribs);
    #else
    printf("%s%s:\x1b[0m ", attribs[level], report_names[level]);
    #endif
}

static void display_line(Cuik_ReportLevel level, TokenStream* tokens, SourceLoc* loc) {
    SourceLocIndex loci = 0;
    while (loc->line->filepath[0] == '<' && loc->line->parent != 0) {
        loci = loc->line->parent;
        loc = GET_SOURCE_LOC(loci);
    }

    SET_COLOR_WHITE;
    printf("%s:%d:%d: ", loc->line->filepath, loc->line->line, loc->columns);
    print_level_name(level);
}

static void tally_report_counter(Cuik_ReportLevel level, Cuik_ErrorStatus* err) {
    if (err == NULL) {
        if (level >= REPORT_ERROR) {
            SET_COLOR_RED;
            printf("ABORTING!!! (no diagnostics callback)\n");
            RESET_COLOR;
            abort();
        }
    } else {
        atomic_fetch_add((atomic_int*) &err->tally[level], 1);
    }
}

static size_t draw_line(TokenStream* tokens, SourceLocIndex loc_index) {
    SourceLine* line = GET_SOURCE_LOC(loc_index)->line;

    // display line
    const char* line_start = (const char*)line->line_str;
    while (*line_start && isspace(*line_start)) {
        line_start++;
    }
    size_t dist_from_line_start = line_start - (const char*)line->line_str;

    // Draw line preview
    if (*line_start != '\r' && *line_start != '\n') {
        const char* line_end = line_start;
        // printf("    ");
        printf("%4d| ", line->line);
        do {
            putchar(*line_end != '\t' ? *line_end : ' ');
            line_end++;
        } while (*line_end && *line_end != '\n');
        printf("\n");

        RESET_COLOR;
    }

    return dist_from_line_start;
}

static void draw_line_horizontal_pad() {
    printf("      ");
}

static SourceLoc merge_source_locations(TokenStream* tokens, SourceLocIndex starti, SourceLocIndex endi) {
    //starti = try_for_nicer_loc(tokens, starti);
    //endi = try_for_nicer_loc(tokens, endi);

    const SourceLoc* start = GET_SOURCE_LOC(starti);
    const SourceLoc* end = GET_SOURCE_LOC(endi);

    if (start->line->filepath != end->line->filepath &&
        start->line->line != end->line->line) {
        return *start;
    }

    // We can only merge if it's on the same line... for now...
    size_t start_columns = start->columns;
    size_t end_columns = end->columns + end->length;
    if (start_columns >= end_columns) {
        return *start;
    }

    return (SourceLoc){ .line = start->line, .columns = start_columns, .length = end_columns - start_columns};
}

static int print_backtrace(TokenStream* tokens, SourceLocIndex loc_index, SourceLine* kid) {
    SourceLoc* loc = GET_SOURCE_LOC(loc_index);
    SourceLine* line = loc->line;

    int line_bias = 0;
    if (line->parent != 0) {
        line_bias = print_backtrace(tokens, line->parent, line);
    }

    switch (loc->type) {
        case SOURCE_LOC_MACRO: {
            if (line->filepath[0] == '<') {
                printf("In macro '%.*s' expanded at line %d:\n", (int)loc->length, line->line_str + loc->columns, line_bias + line->line);
            } else {
                printf("In macro '%.*s' expanded at %s:%d:%d:\n", (int)loc->length, line->line_str + loc->columns, line->filepath, line->line, loc->columns);
            }

            if (!report_using_thin_errors) {
                // draw macro highlight
                size_t dist_from_line_start = draw_line(tokens, loc_index);
                draw_line_horizontal_pad();

                // idk man
                size_t start_pos = loc->columns > dist_from_line_start ? loc->columns - dist_from_line_start : 0;

                // draw underline
                SET_COLOR_GREEN;
                // printf("\x1b[32m");

                size_t tkn_len = loc->length;
                for (size_t i = 0; i < start_pos; i++) printf(" ");
                printf("^");
                for (size_t i = 1; i < tkn_len; i++) printf("~");

                // printf("\x1b[0m");
                printf("\n");
                RESET_COLOR;
            }
            return line_bias;
        }

        default:
        printf("In file %s:%d:\n", line->filepath, line->line);
        return line->line;
    }
}

DiagWriter diag_writer(TokenStream* tokens) {
    return (DiagWriter){ .tokens = tokens, .base = UINT32_MAX };
}

static void diag_writer_write_upto(DiagWriter* writer, size_t pos) {
    if (writer->cursor < pos) {
        int l = pos - writer->cursor;
        for (int i = 0; i < l; i++) printf(" ");
        //printf("%.*s", (int)(pos - writer->cursor), writer->line_start + writer->cursor);
        writer->cursor = pos;
    }
}

void diag_writer_highlight(DiagWriter* writer, SourceLocIndex loc_index) {
    SourceLoc* loc = &writer->tokens->locations[loc_index];
    if (writer->base == UINT32_MAX) {
        SourceLine* line = loc->line;
        const char* line_start = (const char*)line->line_str;
        while (*line_start && isspace(*line_start)) {
            line_start++;
        }

        const char* line_end = line_start;
        do {
            line_end++;
        } while (*line_end && *line_end != '\n');

        writer->base = loc_index;
        writer->line_start = line_start;
        writer->line_end = line_end;
        writer->dist_from_line_start = line_start - (const char*)line->line_str;

        printf("%s:%d\n", line->filepath, line->line);
        draw_line_horizontal_pad();
        printf("%.*s\n", (int) (line_end - line_start), line_start);
        draw_line_horizontal_pad();
    }

    size_t start_pos = loc->columns > writer->dist_from_line_start ? loc->columns - writer->dist_from_line_start : 0;
    size_t tkn_len = loc->length;

    diag_writer_write_upto(writer, start_pos);
    //printf("\x1b[7m");
    //diag_writer_write_upto(writer, start_pos + tkn_len);
    printf("\x1b[32m^");
    for (int i = 1; i < tkn_len; i++) printf("~");
    writer->cursor = start_pos + tkn_len;
    printf("\x1b[0m");
}

bool diag_writer_is_compatible(DiagWriter* writer, SourceLocIndex loc) {
    if (writer->base == UINT32_MAX) {
        return true;
    }

    SourceLine* line1 = writer->tokens->locations[writer->base].line;
    SourceLine* line2 = writer->tokens->locations[loc].line;
    return line1 == line2;
}

void diag_writer_done(DiagWriter* writer) {
    if (writer->base != UINT32_MAX) {
        diag_writer_write_upto(writer, writer->line_end - writer->line_start);
        printf("\n");
    }
}

static void highlight_line(TokenStream* tokens, SourceLocIndex loc_index, SourceLoc* loc) {
    SourceLine* line = GET_SOURCE_LOC(loc_index)->line;

    // display line
    const char* line_start = (const char*)line->line_str;
    while (*line_start && isspace(*line_start)) {
        line_start++;
    }
    size_t dist_from_line_start = line_start - (const char*)line->line_str;

    // Layout token
    size_t start_pos = loc->columns > dist_from_line_start ? loc->columns - dist_from_line_start : 0;
    size_t tkn_len = loc->length;

    // Draw line preview
    if (*line_start != '\r' && *line_start != '\n') {
        const char* line_end = line_start;
        do {
            line_end++;
        } while (*line_end && *line_end != '\n');
        size_t line_len = line_end - line_start;

        printf("%4d| ", line->line);
        printf("%.*s", (int) start_pos, line_start);
        printf("%.*s", (int) tkn_len, line_start + start_pos);
        RESET_COLOR;
        printf("%.*s", (int) (line_len - (start_pos + tkn_len)), line_start + start_pos + tkn_len);
        printf("\n");
    }
}

static void preview_line(TokenStream* tokens, SourceLocIndex loc_index, SourceLoc* loc, const char* tip) {
    if (!report_using_thin_errors) {
        size_t dist_from_line_start = draw_line(tokens, loc_index);
        draw_line_horizontal_pad();

        SET_COLOR_GREEN;

        // idk man
        size_t start_pos = loc->columns > dist_from_line_start ? loc->columns - dist_from_line_start : 0;
        size_t tkn_len = loc->length;
        if (tip) {
            start_pos += loc->length;
            tkn_len = strlen(tip);
        }

        // draw underline
        for (size_t i = 0; i < start_pos; i++) printf(" ");
        printf("^");
        for (size_t i = 1; i < tkn_len; i++) printf("~");
        printf("\n");

        if (tip) {
            draw_line_horizontal_pad();
            for (size_t i = 0; i < start_pos; i++) printf(" ");
            printf("%s\n", tip);
        }

        RESET_COLOR;
    }
}

static void preview_expansion(TokenStream* tokens, SourceLoc* loc) {
    if (loc->line->parent != 0) {
        SourceLoc* parent = GET_SOURCE_LOC(loc->line->parent);

        if (parent->expansion != 0) {
            SourceLoc* expansion = GET_SOURCE_LOC(parent->expansion);

            display_line(REPORT_INFO, tokens, expansion);
            printf("macro '%.*s' defined at\n", (int)expansion->length, expansion->line->line_str + expansion->columns);
            preview_line(tokens, parent->expansion, expansion, NULL);
        }
    }
    printf("\n");
}

void report_header(Cuik_ReportLevel level, const char* fmt, ...) {
    print_level_name(level);

    SET_COLOR_WHITE;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    RESET_COLOR;
}

void report_line(TokenStream* tokens, SourceLocIndex loci, int indent) {
    SourceLoc* loc = GET_SOURCE_LOC(loci);
    while (loc->line->filepath[0] == '<' && loc->line->parent != 0) {
        loci = loc->line->parent;
        loc = GET_SOURCE_LOC(loci);
    }

    for (int i = 0; i < indent; i++) printf(" ");
    printf("%s:%d:%d\n", loc->line->filepath, loc->line->line, loc->columns);
    highlight_line(tokens, loci, loc);
}

void report_ranged(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLocIndex start_loc, SourceLocIndex end_loc, const char* fmt, ...) {
    SourceLoc loc = merge_source_locations(tokens, start_loc, end_loc);

    mtx_lock(&report_mutex);
    if (!report_using_thin_errors && loc.line->parent != 0) {
        print_backtrace(tokens, loc.line->parent, loc.line);
    }

    display_line(level, tokens, &loc);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    RESET_COLOR;

    preview_line(tokens, start_loc, &loc, NULL);
    preview_expansion(tokens, &loc);

    tally_report_counter(level, err);
    mtx_unlock(&report_mutex);
}

void report(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLocIndex loc_index, const char* fmt, ...) {
    SourceLoc* loc = GET_SOURCE_LOC(loc_index);

    mtx_lock(&report_mutex);
    if (!report_using_thin_errors && loc->line->parent != 0) {
        print_backtrace(tokens, loc->line->parent, loc->line);
    }

    display_line(level, tokens, loc);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    RESET_COLOR;

    preview_line(tokens, loc_index, loc, NULL);
    preview_expansion(tokens, loc);

    tally_report_counter(level, err);
    mtx_unlock(&report_mutex);
}

void report_fix(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLocIndex loc_index, const char* tip, const char* fmt, ...) {
    SourceLoc* loc = GET_SOURCE_LOC(loc_index);

    mtx_lock(&report_mutex);
    if (!report_using_thin_errors && loc->line->parent != 0) {
        print_backtrace(tokens, loc->line->parent, loc->line);
    }

    display_line(level, tokens, loc);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    RESET_COLOR;

    preview_line(tokens, loc_index, loc, tip);
    preview_expansion(tokens, loc);

    if (loc->line->parent != 0) {
        SourceLoc* parent = GET_SOURCE_LOC(loc->line->parent);
        if (parent->expansion != 0) {
            report(level, err, tokens, parent->expansion, "Expanded from");
        }
    }

    tally_report_counter(level, err);
    mtx_unlock(&report_mutex);
}

void report_two_spots(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLocIndex loc_index, SourceLocIndex loc2_index, const char* msg, const char* loc_msg, const char* loc_msg2, const char* interjection) {
    SourceLoc* loc = GET_SOURCE_LOC(loc_index);
    SourceLoc* loc2 = GET_SOURCE_LOC(loc2_index);

    mtx_lock(&report_mutex);

    if (!interjection && loc->line->line == loc2->line->line) {
        assert(loc->columns < loc2->columns);

        display_line(level, tokens, loc);
        printf("%s\n", msg);
        RESET_COLOR;

        if (!report_using_thin_errors) {
            size_t dist_from_line_start = draw_line(tokens, loc_index);
            draw_line_horizontal_pad();

            #if _WIN32
            SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | FOREGROUND_GREEN);
            #endif

            // draw underline
            size_t first_start_pos = loc->columns > dist_from_line_start ? loc->columns - dist_from_line_start : 0;
            size_t first_end_pos = first_start_pos + loc->length;

            size_t second_start_pos = loc2->columns > dist_from_line_start ? loc2->columns - dist_from_line_start : 0;
            size_t second_end_pos = second_start_pos + loc2->length;

            // First
            for (size_t i = 0; i < first_start_pos; i++) printf(" ");
            printf("^");
            for (size_t i = first_start_pos + 1; i < first_end_pos; i++) printf("~");

            // Second
            for (size_t i = first_end_pos; i < second_start_pos; i++) printf(" ");
            printf("^");
            for (size_t i = second_start_pos + 1; i < second_end_pos; i++) printf("~");
            printf("\n");

            #if _WIN32
            SetConsoleTextAttribute(console_handle, default_attribs);
            #endif

            draw_line_horizontal_pad();

            size_t loc_msg_len = strlen(loc_msg);
            //size_t loc_msg2_len = strlen(loc_msg2);

            for (size_t i = 0; i < first_start_pos; i++) printf(" ");
            printf("%s", loc_msg);
            for (size_t i = first_start_pos + loc_msg_len; i < second_start_pos; i++) printf(" ");
            printf("%s", loc_msg2);
            printf("\n");
        }
    } else {
        display_line(level, tokens, loc);
        printf("%s\n", msg);

        if (!report_using_thin_errors) {
            {
                size_t dist_from_line_start = draw_line(tokens, loc_index);
                draw_line_horizontal_pad();

                #if _WIN32
                SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | FOREGROUND_GREEN);
                #endif

                // draw underline
                size_t start_pos = loc->columns > dist_from_line_start ? loc->columns - dist_from_line_start : 0;

                size_t tkn_len = loc->length;
                for (size_t i = 0; i < start_pos; i++) printf(" ");
                printf("^");
                for (size_t i = 1; i < tkn_len; i++) printf("~");
                printf("\n");

                #if _WIN32
                SetConsoleTextAttribute(console_handle, default_attribs);
                #endif

                if (loc_msg) {
                    draw_line_horizontal_pad();
                    for (size_t i = 0; i < start_pos; i++) printf(" ");
                    printf("%s\n", loc_msg);
                }
            }

            if (loc->line->filepath != loc2->line->filepath) {
                printf("  meanwhile in... %s\n", loc2->line->filepath);
                draw_line_horizontal_pad();
                printf("\n");
            }

            if (interjection) {
                printf("  %s\n", interjection);
                draw_line_horizontal_pad();
                printf("\n");
            } else {
                draw_line_horizontal_pad();
                printf("\n");
            }

            {
                size_t dist_from_line_start = draw_line(tokens, loc2_index);
                draw_line_horizontal_pad();

                #if _WIN32
                SetConsoleTextAttribute(console_handle, (default_attribs & ~0xF) | FOREGROUND_GREEN);
                #endif

                // draw underline
                size_t start_pos = loc2->columns > dist_from_line_start
                    ? loc2->columns - dist_from_line_start : 0;

                size_t tkn_len = loc2->length;
                for (size_t i = 0; i < start_pos; i++) printf(" ");
                printf("^");
                for (size_t i = 1; i < tkn_len; i++) printf("~");
                printf("\n");

                #if _WIN32
                SetConsoleTextAttribute(console_handle, default_attribs);
                #endif

                if (loc_msg2) {
                    draw_line_horizontal_pad();
                    for (size_t i = 0; i < start_pos; i++) printf(" ");
                    printf("%s\n", loc_msg2);
                }
            }
        }
    }

    printf("\n\n");
    tally_report_counter(level, err);
    mtx_unlock(&report_mutex);
}

bool has_reports(Cuik_ReportLevel minimum, Cuik_ErrorStatus* err) {
    for (int i = minimum; i < REPORT_MAX; i++) {
        if (atomic_load((atomic_int*) &err->tally[i]) > 0) {
            //printf("exited with %d %s%s", tally[i], report_names[i], tally[i] > 1 ? "s" : "");
            return true;
        }
    }

    return false;
}
