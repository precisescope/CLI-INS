#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define APP_NAME "CLI-INS"
#define APP_VERSION "6.1"
#define START_DIR "/storage/emulated/0"
#define MAX_DEVICES 256
#define MAX_ENTRIES 4096
#define PATH_SIZE 4096
#define INPUT_SIZE 256
#define SERIAL_SIZE 512
#define OUTPUT_SIZE 65536
#define LIST_TOP 8
#define CODE_LEN 6

enum {
    CP_NORMAL = 1,
    CP_TITLE,
    CP_SELECTED,
    CP_MUTED,
    CP_ACCENT,
    CP_BORDER,
    CP_INPUT
};

typedef struct {
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
} Device;

typedef struct {
    char name[INPUT_SIZE];
    int is_dir;
    int is_apk;
    int is_link;
    int is_broken;
    int is_special;
    off_t size;
} Entry;

typedef struct {
    char serial[SERIAL_SIZE];
    char state[32];
} ADBDevice;

static ADBDevice adb_devices[MAX_DEVICES];
static int adb_device_count;
static int adb_usable;
static char adb_problem[300];
static char adb_output[OUTPUT_SIZE];
static int phonebook_skipped;

typedef struct {
    char name[32];
    short fg;
    short bg;
    short accent;
    short sel_fg;
    short sel_bg;
    short in_fg;
    short in_bg;
} Theme;

static Device devices[MAX_DEVICES];
static int device_count;

static Entry entries[MAX_ENTRIES];
static int entry_count;
static int show_hidden;
static int show_all_files;

static char home_dir[PATH_SIZE];
static char config_dir[PATH_SIZE];
static char phonebook_path[PATH_SIZE];
static char theme_path[PATH_SIZE];

static int use_colors;
static int default_bg_ok;
static Theme current_theme;

/* ---------------------------------------------------------
   Themes
   --------------------------------------------------------- */

static const Theme presets[] = {
    {"Aero",    COLOR_WHITE,  COLOR_BLACK, COLOR_CYAN,    COLOR_BLACK, COLOR_CYAN,    COLOR_WHITE, COLOR_BLUE},
    {"Matrix",  COLOR_GREEN,  COLOR_BLACK, COLOR_GREEN,   COLOR_BLACK, COLOR_GREEN,   COLOR_BLACK, COLOR_GREEN},
    {"Amber",   COLOR_YELLOW, COLOR_BLACK, COLOR_YELLOW,  COLOR_BLACK, COLOR_YELLOW,  COLOR_BLACK, COLOR_YELLOW},
    {"Ocean",   COLOR_WHITE,  COLOR_BLUE,  COLOR_CYAN,    COLOR_BLUE,  COLOR_WHITE,   COLOR_WHITE, COLOR_CYAN},
    {"Magenta", COLOR_WHITE,  COLOR_BLACK, COLOR_MAGENTA, COLOR_BLACK, COLOR_MAGENTA, COLOR_WHITE, COLOR_MAGENTA},
    {"Mono",    COLOR_WHITE,  -1,          COLOR_WHITE,   COLOR_BLACK, COLOR_WHITE,   COLOR_BLACK, COLOR_WHITE},
    {"Paper",   COLOR_BLACK,  COLOR_WHITE, COLOR_BLUE,    COLOR_WHITE, COLOR_BLUE,    COLOR_WHITE, COLOR_BLUE},
};

#define PRESET_COUNT ((int)(sizeof(presets) / sizeof(presets[0])))

static const char *color_names[] = {
    "Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"
};

#define MENU_CANCEL (-1)

/* Too small for the forms and file browser to draw without clipping. */
static int screen_too_small(int rows, int cols)
{
    return rows < 14 || cols < 40;
}

static int menu(const char *title, const char *items[], int count);
static void message_page(const char *title, const char *message);
static int join_path(char *dst, size_t size, const char *dir, const char *name);

static short resolve_bg(short c)
{
    if (c < 0 && !default_bg_ok)
        return COLOR_BLACK;
    return c;
}

static void apply_theme(const Theme *t)
{
    current_theme = *t;

    if (!use_colors)
        return;

    short bg = resolve_bg(t->bg);

    init_pair(CP_NORMAL, t->fg, bg);
    init_pair(CP_TITLE, t->fg, bg);
    init_pair(CP_SELECTED, t->sel_fg, t->sel_bg);
    init_pair(CP_MUTED, t->accent, bg);
    init_pair(CP_ACCENT, t->accent, bg);
    init_pair(CP_BORDER, t->accent, bg);
    init_pair(CP_INPUT, t->in_fg, t->in_bg);

    bkgd(COLOR_PAIR(CP_NORMAL));
}


/* Writes data to a temporary file, flushes it to disk, then renames it over
   path. Readers see either the old file or the complete new one. */
static int atomic_write(const char *path, const char *data, size_t length)
{
    char temporary[PATH_SIZE];
    size_t written = 0;
    int ok = 1;
    int fd;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path)
        >= (int)sizeof(temporary))
        return -1;

    fd = mkstemp(temporary);

    if (fd < 0)
        return -1;

    if (fchmod(fd, 0600) != 0)
        ok = 0;

    while (ok && written < length) {
        ssize_t n = write(fd, data + written, length - written);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = 0;
        } else {
            written += (size_t)n;
        }
    }

    if (ok && fsync(fd) != 0)
        ok = 0;

    if (close(fd) != 0)
        ok = 0;

    if (ok && rename(temporary, path) != 0)
        ok = 0;

    if (!ok)
        unlink(temporary);

    return ok ? 0 : -1;
}

static int save_theme(void)
{
    char *buffer = NULL;
    size_t length = 0;
    FILE *memory = open_memstream(&buffer, &length);
    int result;

    if (!memory)
        return -1;

    fprintf(memory, "%s\n%d %d %d %d %d %d %d\n",
            current_theme.name,
            current_theme.fg, current_theme.bg, current_theme.accent,
            current_theme.sel_fg, current_theme.sel_bg,
            current_theme.in_fg, current_theme.in_bg);

    if (fclose(memory) != 0) {
        free(buffer);
        return -1;
    }

    result = atomic_write(theme_path, buffer, length);
    free(buffer);
    return result;
}

/* Applies the theme for this session, then saves it. A failed save is
   reported, but the theme stays active until the app exits. */
static void apply_and_save_theme(const Theme *t)
{
    apply_theme(t);

    if (save_theme() != 0)
        message_page("THEME NOT SAVED",
                     "Applied for this session only. Check storage space and permissions.");
}

static int valid_color(int c)
{
    return c >= -1 && c <= 7;
}

static void load_theme(void)
{
    Theme t = presets[0];
    char line[256];
    int v[7];
    FILE *fp = fopen(theme_path, "r");

    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            snprintf(t.name, sizeof(t.name), "%.31s", line);
        }

        if (fgets(line, sizeof(line), fp) &&
            sscanf(line, "%d %d %d %d %d %d %d",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7 &&
            valid_color(v[0]) && valid_color(v[1]) && valid_color(v[2]) &&
            valid_color(v[3]) && valid_color(v[4]) && valid_color(v[5]) &&
            valid_color(v[6])) {
            t.fg = (short)v[0];
            t.bg = (short)v[1];
            t.accent = (short)v[2];
            t.sel_fg = (short)v[3];
            t.sel_bg = (short)v[4];
            t.in_fg = (short)v[5];
            t.in_bg = (short)v[6];
        }

        fclose(fp);
    }

    apply_theme(&t);
}


static void custom_theme(void)
{
    const char *items[9];
    int text;
    int bg;
    int accent;

    for (int i = 0; i < 8; i++)
        items[i] = color_names[i];

    items[8] = "Cancel";

    text = menu("CUSTOM: TEXT COLOR", items, 9);
    if (text < 0 || text >= 8)
        return;

    bg = menu("CUSTOM: BACKGROUND", items, 9);
    if (bg < 0 || bg >= 8)
        return;

    if (text == bg) {
        message_page("INVALID THEME", "Text and background cannot be the same color.");
        return;
    }

    accent = menu("CUSTOM: ACCENT COLOR", items, 9);
    if (accent < 0 || accent >= 8)
        return;

    Theme t = {
        "Custom",
        (short)text, (short)bg, (short)accent,
        COLOR_BLACK, (short)accent,
        COLOR_WHITE, COLOR_BLUE
    };

    apply_and_save_theme(&t);
}


static void theme_menu(void)
{
    const char *items[PRESET_COUNT + 2];
    char title[64];
    int n = PRESET_COUNT;

    for (int i = 0; i < n; i++)
        items[i] = presets[i].name;

    items[n] = "Custom...";
    items[n + 1] = "Back";

    while (1) {
        snprintf(title, sizeof(title), "THEME (current: %s)", current_theme.name);

        int sel = menu(title, items, n + 2);

        if (sel < 0 || sel == n + 1)
            return;

        if (sel == n) {
            custom_theme();
        } else {
            Theme t = presets[sel];
            apply_and_save_theme(&t);
        }
    }
}

/* ---------------------------------------------------------
   Configuration and validation
   --------------------------------------------------------- */

static int make_directory(const char *path)
{
    struct stat st;

    if (mkdir(path, 0700) == 0)
        return 0;

    if (errno != EEXIST)
        return -1;

    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;

    return 0;
}

static int ensure_config_dir(const char *home)
{
    char path[PATH_SIZE];

    if (snprintf(path, sizeof(path), "%s/.config", home) >= (int)sizeof(path))
        return -1;

    if (make_directory(path) != 0)
        return -1;

    return make_directory(config_dir);
}


/* Removes leading and trailing whitespace in place. */
static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;

    while (start < len && isspace((unsigned char)s[start]))
        start++;

    if (start > 0)
        memmove(s, s + start, len - start + 1);

    len = strlen(s);

    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/* IPv4 dotted-quad only. Hostnames and IPv6 addresses are rejected on
   purpose: see the README for the reasoning. */
static int valid_host(const char *host)
{
    struct in_addr addr;

    if (!host || !*host || strlen(host) >= INPUT_SIZE)
        return 0;

    for (const char *p = host; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.')
            return 0;
    }

    return inet_pton(AF_INET, host, &addr) == 1;
}

static int valid_port(const char *port)
{
    if (!port || !*port)
        return 0;

    for (const char *p = port; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(port, &end, 10);

    return errno == 0 && end && !*end && value >= 1 && value <= 65535;
}


static int valid_code(const char *code)
{
    if (!code || strlen(code) != CODE_LEN)
        return 0;

    for (size_t i = 0; i < CODE_LEN; i++) {
        if (!isdigit((unsigned char)code[i]))
            return 0;
    }

    return 1;
}


static int safe_name(const char *name)
{
    size_t len;

    if (!name)
        return 0;

    len = strlen(name);

    if (len == 0 || len >= INPUT_SIZE)
        return 0;

    if (isspace((unsigned char)name[0]) || isspace((unsigned char)name[len - 1]))
        return 0;

    for (const char *p = name; *p; p++) {
        if (*p == '|' || iscntrl((unsigned char)*p))
            return 0;
    }

    return 1;
}

static void secure_wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;

    while (n--)
        *v++ = 0;
}

/* ---------------------------------------------------------
   Phonebook
   --------------------------------------------------------- */


static int save_phonebook(void)
{
    char *buffer = NULL;
    size_t length = 0;
    FILE *memory = open_memstream(&buffer, &length);
    int result;

    if (!memory)
        return -1;

    for (int i = 0; i < device_count; i++)
        fprintf(memory, "%s\t%s\n", devices[i].name, devices[i].host);

    if (fclose(memory) != 0) {
        free(buffer);
        return -1;
    }

    result = atomic_write(phonebook_path, buffer, length);
    free(buffer);
    return result;
}


static int host_is_saved(const char *host)
{
    for (int i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].host, host))
            return 1;
    }

    return 0;
}

/* Reads one entry per line: name<TAB>ipv4. Anything else is skipped and
   counted in phonebook_skipped, so a damaged file never crashes the app. */
static void load_phonebook(void)
{
    char line[INPUT_SIZE * 2 + 16];
    FILE *fp;

    device_count = 0;
    phonebook_skipped = 0;

    fp = fopen(phonebook_path, "r");

    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        char *tab;

        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        } else if (!feof(fp)) {
            int c;

            /* Line longer than the buffer: discard the rest of it. */
            while ((c = fgetc(fp)) != '\n' && c != EOF)
                ;

            phonebook_skipped++;
            continue;
        }

        line[strcspn(line, "\r")] = '\0';

        if (line[0] == '\0')
            continue;

        tab = strchr(line, '\t');

        if (!tab || strchr(tab + 1, '\t')) {
            phonebook_skipped++;
            continue;
        }

        *tab = '\0';

        if (!safe_name(line) || !valid_host(tab + 1) || host_is_saved(tab + 1)
            || device_count >= MAX_DEVICES) {
            phonebook_skipped++;
            continue;
        }

        snprintf(devices[device_count].name, sizeof(devices[device_count].name), "%s", line);
        snprintf(devices[device_count].host, sizeof(devices[device_count].host), "%s", tab + 1);
        device_count++;
    }

    fclose(fp);
}

static int save_device(const char *name, const char *host)
{
    if (!safe_name(name) || !valid_host(host))
        return -1;

    for (int i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].host, host)) {
            Device old = devices[i];

            snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);

            if (save_phonebook() != 0) {
                devices[i] = old;
                return -1;
            }

            return 0;
        }
    }

    if (device_count >= MAX_DEVICES)
        return -1;

    snprintf(devices[device_count].name, sizeof(devices[device_count].name), "%s", name);
    snprintf(devices[device_count].host, sizeof(devices[device_count].host), "%s", host);
    device_count++;

    if (save_phonebook() != 0) {
        device_count--;
        return -1;
    }

    return 0;
}

static int remove_device(int index)
{
    if (index < 0 || index >= device_count)
        return -1;

    Device *backup = malloc(sizeof(devices));

    if (!backup)
        return -1;

    memcpy(backup, devices, sizeof(devices));

    int old_count = device_count;

    for (int i = index; i < device_count - 1; i++)
        devices[i] = devices[i + 1];

    device_count--;

    if (save_phonebook() != 0) {
        memcpy(devices, backup, sizeof(devices));
        device_count = old_count;
        free(backup);
        return -1;
    }

    free(backup);
    return 0;
}

/* ---------------------------------------------------------
   Drawing helpers
   --------------------------------------------------------- */

static void color_on(int pair)
{
    if (use_colors)
        attron(COLOR_PAIR(pair));
}

static void color_off(int pair)
{
    if (use_colors)
        attroff(COLOR_PAIR(pair));
}

static void centered(int y, const char *text, int pair)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    (void)rows;

    int x = (cols - (int)strlen(text)) / 2;

    if (x < 0)
        x = 0;

    color_on(pair);
    mvaddnstr(y, x, text, cols - x);
    color_off(pair);
}

static void draw_box(int y, int x, int h, int w)
{
    if (h < 2 || w < 2)
        return;

    color_on(CP_BORDER);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

    for (int i = 1; i < w - 1; i++) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + h - 1, x + i, ACS_HLINE);
    }

    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }

    color_off(CP_BORDER);
}

static void header(const char *subtitle)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    erase();

    centered(1, APP_NAME, CP_TITLE);
    centered(2, "LIGHTWEIGHT CLI APK INSTALLER", CP_MUTED);

    if (rows > 4 && cols > 8) {
        color_on(CP_BORDER);
        mvhline(4, 2, ACS_HLINE, cols - 4);
        color_off(CP_BORDER);
    }

    if (subtitle && rows > 6)
        centered(6, subtitle, CP_ACCENT);
}

static void footer_lines(const char *first, const char *second)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    (void)cols;

    if (rows < 6)
        return;

    centered(rows - 4, first, CP_MUTED);
    centered(rows - 3, second, CP_MUTED);
}

static void footer(void)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    (void)rows;

    footer_lines("ARROWS: Navigate   ENTER: Select   ESC: Back",
                 cols >= 60
                     ? "Made by precisescope (GitHub) | 8bit_binary (Instagram)"
                     : "GitHub: precisescope");
}

/* ---------------------------------------------------------
   Messages and input
   --------------------------------------------------------- */

static void pause_message(const char *message)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);

    if (rows > 3) {
        color_on(CP_MUTED);
        mvaddnstr(rows - 4, 2, message, cols - 4);
        mvaddstr(rows - 2, 2, "Press any key to continue...");
        color_off(CP_MUTED);
    }

    refresh();
    (void)getch();
}

static void message_page(const char *title, const char *message)
{
    header(title);

    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    (void)cols;

    if (rows > 8)
        centered(rows / 2, message, CP_ACCENT);

    footer();
    refresh();
    pause_message(" ");
}


static int read_field(const char *label, char *buffer, size_t size)
{
    int rows;
    int cols;
    int max_len;
    int result;

    getmaxyx(stdscr, rows, cols);

    if (size < 2) {
        return 0;
    }

    if (screen_too_small(rows, cols)) {
        message_page("TERMINAL TOO SMALL", "Resize the terminal to at least 40 x 14.");
        return 0;
    }

    header("INPUT");

    if (rows > 8) {
        color_on(CP_TITLE);
        mvaddnstr(8, 4, label, cols - 8);
        color_off(CP_TITLE);
    }

    color_on(CP_INPUT);
    mvaddstr(10, 4, "> ");
    color_off(CP_INPUT);

    buffer[0] = '\0';

    max_len = cols - 8;

    if (max_len > (int)size - 1)
        max_len = (int)size - 1;

    echo();
    curs_set(1);
    move(10, 6);
    clrtoeol();

    result = getnstr(buffer, max_len);

    noecho();
    curs_set(0);

    buffer[size - 1] = '\0';

    return result != ERR;
}


/* Reads exactly CODE_LEN digits. Digits are shown as '*'. Escape wipes the
   buffer and cancels. */
static int read_secret_field(const char *label, char *buffer, size_t size)
{
    int rows;
    int cols;
    size_t used = 0;

    getmaxyx(stdscr, rows, cols);

    if (size < CODE_LEN + 1) {
        return 0;
    }

    if (screen_too_small(rows, cols)) {
        secure_wipe(buffer, size);
        message_page("TERMINAL TOO SMALL", "Resize the terminal to at least 40 x 14.");
        return 0;
    }

    header("PAIRING CODE");

    if (rows > 8) {
        color_on(CP_TITLE);
        mvaddnstr(8, 4, label, cols - 8);
        color_off(CP_TITLE);
    }

    color_on(CP_INPUT);
    mvaddstr(10, 4, "> ");
    color_off(CP_INPUT);

    buffer[0] = '\0';

    curs_set(1);
    move(10, 6);
    refresh();

    while (1) {
        int ch = getch();

        if (ch == '\n' || ch == KEY_ENTER) {
            if (used == CODE_LEN)
                break;
            continue;
        }

        if (ch == 27) {
            secure_wipe(buffer, size);
            curs_set(0);
            return 0;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (used > 0) {
                int y;
                int x;

                used--;
                buffer[used] = '\0';
                getyx(stdscr, y, x);
                mvaddch(y, x - 1, ' ');
                move(y, x - 1);
                refresh();
            }
            continue;
        }

        if (ch >= '0' && ch <= '9' && used < CODE_LEN) {
            buffer[used++] = (char)ch;
            buffer[used] = '\0';
            addch('*');
            refresh();
        }
    }

    curs_set(0);
    return 1;
}

/* ---------------------------------------------------------
   Menu
   --------------------------------------------------------- */

static int menu(const char *title, const char *items[], int count)
{
    int selected = 0;
    int offset = 0;

    if (count <= 0)
        return -1;

    while (1) {
        int rows, cols;

        getmaxyx(stdscr, rows, cols);
        header(title);

        if (rows < 19 || cols < 36) {
            centered(rows / 2, "Terminal too small - resize it.", CP_ACCENT);
            refresh();

            int key = getch();

            if (key == 27 || key == 'q' || key == 'Q')
                return MENU_CANCEL;

            continue;
        }

        int panel_width = cols - 8;

        if (panel_width > 72)
            panel_width = 72;

        int panel_x = (cols - panel_width) / 2;
        int panel_y = 8;
        int available_height = rows - panel_y - 7;
        int panel_height = count + 3;

        if (panel_height > available_height)
            panel_height = available_height;

        if (panel_height < 4)
            panel_height = 4;

        int visible = panel_height - 3;

        if (visible > count)
            visible = count;

        if (selected < offset)
            offset = selected;

        if (selected >= offset + visible)
            offset = selected - visible + 1;

        draw_box(panel_y, panel_x, panel_height, panel_width);

        color_on(CP_MUTED);
        mvaddnstr(panel_y + 1, panel_x + 3, "SELECT AN OPTION", panel_width - 6);
        color_off(CP_MUTED);

        for (int i = 0; i < visible; i++) {
            int index = offset + i;
            int y = panel_y + 2 + i;

            if (index >= count)
                break;

            if (index == selected) {
                color_on(CP_SELECTED);
                mvhline(y, panel_x + 1, ' ', panel_width - 2);
                mvaddnstr(y, panel_x + 3, "> ", 2);
                mvaddnstr(y, panel_x + 5, items[index], panel_width - 8);
                color_off(CP_SELECTED);
            } else {
                color_on(CP_NORMAL);
                mvaddnstr(y, panel_x + 3, "  ", 2);
                mvaddnstr(y, panel_x + 5, items[index], panel_width - 8);
                color_off(CP_NORMAL);
            }
        }

        footer();
        refresh();

        int key = getch();

        switch (key) {
        case KEY_UP:
            selected = (selected + count - 1) % count;
            break;

        case KEY_DOWN:
            selected = (selected + 1) % count;
            break;

        case '\n':
        case KEY_ENTER:
        case KEY_RIGHT:
            return selected;

        case 27:
            return MENU_CANCEL;

        case KEY_RESIZE:
            clear();
            break;

        default:
            if (key == 'q' || key == 'Q')
                return MENU_CANCEL;
            break;
        }
    }
}

/* ---------------------------------------------------------
   External commands
   --------------------------------------------------------- */

static int wait_for_child(pid_t pid)
{
    int status = -1;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}




/* Runs argv with stdin on /dev/null and stdout+stderr captured into output.
   Returns the exit status, or -1 if the child was killed by a signal. */
static int capture_command(const char *const argv[], char *output, size_t output_size)
{
    int pipefd[2];
    size_t used = 0;
    pid_t pid;

    if (!output || output_size < 2)
        return -1;

    output[0] = '\0';

    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();

    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);

        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);

        close(pipefd[1]);

        /* POSIX declares execvp's argv as char *const[], which predates const
           string literals. execvp never writes through argv, so the cast is safe. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
        execvp(argv[0], (char *const *)argv);
#pragma GCC diagnostic pop
        _exit(127);
    }

    close(pipefd[1]);

    while (1) {
        char buffer[4096];
        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (n == 0)
            break;

        if ((size_t)n > output_size - used - 1)
            n = (ssize_t)(output_size - used - 1);

        if (n > 0) {
            memcpy(output + used, buffer, (size_t)n);
            used += (size_t)n;
            output[used] = '\0';
        }
    }

    close(pipefd[0]);

    return wait_for_child(pid);
}

static int have_command(const char *command)
{
    const char *path = getenv("PATH");

    if (!path)
        return 0;

    char *copy = strdup(path);

    if (!copy)
        return 0;

    char *save = NULL;
    char *token = strtok_r(copy, ":", &save);

    while (token) {
        char candidate[PATH_SIZE];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", token, command);

        if (n >= 0 && (size_t)n < sizeof(candidate) && access(candidate, X_OK) == 0) {
            free(copy);
            return 1;
        }

        token = strtok_r(NULL, ":", &save);
    }

    free(copy);
    return 0;
}


static int contains_nocase(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    for (const char *h = haystack; *h; h++) {
        if (strncasecmp(h, needle, n) == 0)
            return 1;
    }

    return 0;
}

/* adb reports most failures as text, sometimes with exit status 0. */
static int output_failed(const char *out)
{
    return contains_nocase(out, "fail")
        || contains_nocase(out, "error")
        || contains_nocase(out, "cannot")
        || contains_nocase(out, "unable")
        || contains_nocase(out, "no such");
}

/* First line of adb output that is not a "* daemon ..." notice. */
static const char *summary_line(const char *out, char *buf, size_t size)
{
    const char *p = out;

    buf[0] = '\0';

    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len > 0 && p[0] != '*') {
            size_t n = len < size - 1 ? len : size - 1;

            memcpy(buf, p, n);
            buf[n] = '\0';
            return buf;
        }

        p = end ? end + 1 : p + len;
    }

    return buf;
}

static void busy(const char *text)
{
    int rows;
    int cols;

    getmaxyx(stdscr, rows, cols);
    (void)cols;
    header("WORKING");

    if (rows > 8)
        centered(rows / 2, text, CP_ACCENT);

    refresh();
}

static int adb_run(const char *const argv[], const char *busy_text)
{
    busy(busy_text);
    return capture_command(argv, adb_output, sizeof(adb_output));
}

static void report_adb_result(const char *ok_title, const char *fail_title,
                              int rc, const char *success_text)
{
    char line[300];

    if (rc == 0 && !output_failed(adb_output)) {
        message_page(ok_title, success_text);
        return;
    }

    summary_line(adb_output, line, sizeof(line));

    if (line[0] == '\0')
        snprintf(line, sizeof(line), "ADB exited with status %d.", rc);

    message_page(fail_title, line);
}

/* Checks once that adb is installed and runs. A failed check is retried on
   the next attempt, so installing adb does not need an app restart. */
static int adb_ready(void)
{
    const char *args[] = { "adb", "version", NULL };
    int rc;

    if (adb_usable)
        return 1;

    if (!have_command("adb")) {
        snprintf(adb_problem, sizeof(adb_problem),
                 "ADB is not installed. Run: pkg install android-tools");
        return 0;
    }

    rc = capture_command(args, adb_output, sizeof(adb_output));

    if (rc == 0 && !output_failed(adb_output)) {
        adb_usable = 1;
        return 1;
    }

    {
        char line[256];

        summary_line(adb_output, line, sizeof(line));
        snprintf(adb_problem, sizeof(adb_problem), "ADB did not start (status %d)%s%s",
                 rc, line[0] ? ": " : "", line);
    }

    return 0;
}

static int check_adb(void)
{
    if (!adb_ready()) {
        message_page("ADB UNAVAILABLE", adb_problem);
        return 0;
    }

    return 1;
}

/* One "adb devices" call per screen. Everything else reads the cache. */
static int adb_refresh(void)
{
    const char *args[] = { "adb", "devices", NULL };
    char *copy;
    char *save = NULL;
    char *line;
    int rc;

    rc = capture_command(args, adb_output, sizeof(adb_output));
    adb_device_count = 0;

    if (rc != 0 || output_failed(adb_output)) {
        char line_text[256];

        summary_line(adb_output, line_text, sizeof(line_text));
        snprintf(adb_problem, sizeof(adb_problem), "adb devices failed (status %d)%s%s",
                 rc, line_text[0] ? ": " : "", line_text);
        return -1;
    }

    copy = strdup(adb_output);

    if (!copy) {
        snprintf(adb_problem, sizeof(adb_problem), "Out of memory.");
        return -1;
    }

    for (line = strtok_r(copy, "\n", &save);
         line && adb_device_count < MAX_DEVICES;
         line = strtok_r(NULL, "\n", &save)) {
        char serial[SERIAL_SIZE];
        char state[32];

        if (line[0] == '*')
            continue;

        if (sscanf(line, "%511s %31s", serial, state) == 2 && strcmp(serial, "List") != 0) {
            snprintf(adb_devices[adb_device_count].serial,
                     sizeof(adb_devices[adb_device_count].serial), "%s", serial);
            snprintf(adb_devices[adb_device_count].state,
                     sizeof(adb_devices[adb_device_count].state), "%s", state);
            adb_device_count++;
        }
    }

    free(copy);
    return 0;
}

/* Finds a cached device by the host part of its serial. Returns -1 if none. */
static int find_adb_device(const char *host)
{
    size_t len = strlen(host);

    for (int i = 0; i < adb_device_count; i++) {
        if (strncmp(adb_devices[i].serial, host, len) == 0
            && adb_devices[i].serial[len] == ':')
            return i;
    }

    return -1;
}

/* ---------------------------------------------------------
   ADB devices
   --------------------------------------------------------- */





static const char *friendly_name(const char *serial)
{
    static char result[INPUT_SIZE];

    for (int i = 0; i < device_count; i++) {
        size_t len = strlen(devices[i].host);

        if (strncmp(serial, devices[i].host, len) == 0 && serial[len] == ':') {
            snprintf(result, sizeof(result), "%s", devices[i].name);
            return result;
        }
    }

    snprintf(result, sizeof(result), "%s", serial);
    return result;
}

/* Returns 1 only when the user explicitly picks the yes option.
   Escape, Q, or the default "No" item never confirm. */
static int confirm_action(const char *prompt, const char *yes_label)
{
    const char *items[] = { "No, cancel", yes_label };

    return menu(prompt, items, 2) == 1;
}

static int confirm_disconnect(const char *name)
{
    char prompt[INPUT_SIZE + 64];

    snprintf(prompt, sizeof(prompt), "Disconnect from %.150s?", name);

    return confirm_action(prompt, "Yes, disconnect");
}


static void pair_device(void)
{
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
    char port[16];
    char code[CODE_LEN + 4];
    char address[INPUT_SIZE + 24];
    const char *args[5];
    int rc;

    memset(code, 0, sizeof(code));

    if (!check_adb())
        return;

    if (!read_field("Friendly device name:", name, sizeof(name)) ||
        !read_field("Device IPv4 address (e.g. 192.168.1.20):", host, sizeof(host)) ||
        !read_field("Pairing port:", port, sizeof(port)))
        return;

    if (!read_secret_field("Six-digit pairing code:", code, sizeof(code))) {
        secure_wipe(code, sizeof(code));
        return;
    }

    trim(name);
    trim(host);
    trim(port);

    if (!safe_name(name) || !valid_host(host) || !valid_port(port) || !valid_code(code)) {
        secure_wipe(code, sizeof(code));
        message_page("INVALID INPUT", "Check the name, IPv4 address, port and six-digit code.");
        return;
    }

    snprintf(address, sizeof(address), "%s:%s", host, port);

    args[0] = "adb";
    args[1] = "pair";
    args[2] = address;
    args[3] = code;
    args[4] = NULL;

    rc = adb_run(args, "Pairing...");

    /* The code only needs to live until adb has read it. */
    secure_wipe(code, sizeof(code));

    if (rc == 0 && !output_failed(adb_output)) {
        if (save_device(name, host) == 0)
            message_page("DEVICE PAIRED", "Paired and saved.");
        else
            message_page("SAVE ERROR", "Paired, but the phonebook could not be saved.");
        return;
    }

    report_adb_result("PAIRED", "PAIRING FAILED", rc, "Paired.");
}


static void connect_saved_device(int index)
{
    Device d;
    int found;
    char port[16];
    char address[INPUT_SIZE + 24];
    const char *args[4];
    int rc;

    if (index < 0 || index >= device_count)
        return;

    d = devices[index];
    found = find_adb_device(d.host);

    if (found >= 0) {
        if (!confirm_disconnect(d.name))
            return;

        args[0] = "adb";
        args[1] = "disconnect";
        args[2] = adb_devices[found].serial;
        args[3] = NULL;

        rc = adb_run(args, "Disconnecting...");
        report_adb_result("DISCONNECTED", "DISCONNECT FAILED", rc, "Device disconnected.");
        return;
    }

    if (!read_field("Wireless debugging connection port:", port, sizeof(port)))
        return;

    trim(port);

    if (!valid_port(port)) {
        message_page("INVALID PORT", "Enter a port between 1 and 65535.");
        return;
    }

    snprintf(address, sizeof(address), "%s:%s", d.host, port);

    args[0] = "adb";
    args[1] = "connect";
    args[2] = address;
    args[3] = NULL;

    rc = adb_run(args, "Connecting...");
    report_adb_result("CONNECTED", "CONNECTION FAILED", rc,
                      "Connected. Use the device list to confirm.");
}


static void saved_device_list(void)
{
    int first = 1;

    while (1) {
        char labels[MAX_DEVICES][INPUT_SIZE + 96];
        const char *items[MAX_DEVICES + 2];
        int selected;

        load_phonebook();

        if (first && phonebook_skipped > 0) {
            char note[160];

            snprintf(note, sizeof(note), "%d invalid saved entr%s ignored.",
                     phonebook_skipped, phonebook_skipped == 1 ? "y was" : "ies were");
            message_page("SAVED DEVICES", note);
        }

        first = 0;

        if (device_count == 0) {
            message_page("SAVED DEVICES", "No saved devices. Pair a device first.");
            return;
        }

        if (adb_refresh() != 0) {
            message_page("ADB ERROR", adb_problem);
            return;
        }

        for (int i = 0; i < device_count; i++) {
            int idx = find_adb_device(devices[i].host);

            if (idx < 0)
                snprintf(labels[i], sizeof(labels[i]), "%s: CONNECT", devices[i].name);
            else if (strcmp(adb_devices[idx].state, "device") == 0)
                snprintf(labels[i], sizeof(labels[i]), "%s: DISCONNECT", devices[i].name);
            else
                snprintf(labels[i], sizeof(labels[i]), "%s: %s - select to disconnect",
                         devices[i].name, adb_devices[idx].state);

            items[i] = labels[i];
        }

        items[device_count] = "Remove a saved device";
        items[device_count + 1] = "Back";

        selected = menu("SAVED DEVICES", items, device_count + 2);

        if (selected < 0 || selected == device_count + 1)
            return;

        if (selected == device_count) {
            const char *remove_items[MAX_DEVICES + 1];
            int target;

            for (int i = 0; i < device_count; i++)
                remove_items[i] = devices[i].name;

            remove_items[device_count] = "Cancel";

            target = menu("REMOVE SAVED DEVICE", remove_items, device_count + 1);

            if (target >= 0 && target < device_count) {
                char prompt[INPUT_SIZE + 64];

                snprintf(prompt, sizeof(prompt),
                         "Remove %.150s from saved devices?", devices[target].name);

                if (confirm_action(prompt, "Yes, remove")) {
                    if (remove_device(target) == 0)
                        message_page("DEVICE REMOVED", "Saved device entry removed.");
                    else
                        message_page("SAVE ERROR", "Could not update saved devices.");
                }
            }

            continue;
        }

        connect_saved_device(selected);
    }
}

/* ---------------------------------------------------------
   APK installation
   --------------------------------------------------------- */

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}




static void install_apk(const char *apk_path)
{
    struct stat st;
    char path_copy[PATH_SIZE];
    int ready[MAX_DEVICES];
    int ready_count = 0;
    char labels[MAX_DEVICES][INPUT_SIZE + SERIAL_SIZE + 8];
    const char *items[MAX_DEVICES + 1];
    int choice;
    char prompt[INPUT_SIZE + 80];
    const char *args[7];
    int rc;

    if (stat(apk_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        message_page("INVALID APK", "The selected file is unavailable.");
        return;
    }

    if (!check_adb())
        return;

    if (adb_refresh() != 0) {
        message_page("ADB ERROR", adb_problem);
        return;
    }

    for (int i = 0; i < adb_device_count; i++) {
        if (strcmp(adb_devices[i].state, "device") == 0)
            ready[ready_count++] = i;
    }

    if (ready_count == 0) {
        if (adb_device_count == 0) {
            message_page("NO CONNECTED DEVICES", "Connect an ADB device before installing.");
        } else {
            char note[INPUT_SIZE + 160];
            int first_bad = 0;

            for (int i = 0; i < adb_device_count; i++) {
                if (strcmp(adb_devices[i].state, "device") != 0) {
                    first_bad = i;
                    break;
                }
            }

            snprintf(note, sizeof(note),
                     "%s is %s. Accept the debugging prompt on the phone, or reconnect.",
                     adb_devices[first_bad].serial, adb_devices[first_bad].state);
            message_page("DEVICE NOT READY", note);
        }
        return;
    }

    for (int k = 0; k < ready_count; k++) {
        snprintf(labels[k], sizeof(labels[k]), "%s | %s",
                 friendly_name(adb_devices[ready[k]].serial),
                 adb_devices[ready[k]].serial);
        items[k] = labels[k];
    }

    items[ready_count] = "Cancel";

    choice = menu("CHOOSE INSTALLATION TARGET", items, ready_count + 1);

    if (choice < 0 || choice >= ready_count)
        return;

    snprintf(path_copy, sizeof(path_copy), "%s", apk_path);
    snprintf(prompt, sizeof(prompt), "Install %.150s?", basename_of(apk_path));

    if (!confirm_action(prompt, "Install"))
        return;

    args[0] = "adb";
    args[1] = "-s";
    args[2] = adb_devices[ready[choice]].serial;
    args[3] = "install";
    args[4] = "-r";
    args[5] = path_copy;
    args[6] = NULL;

    rc = adb_run(args, "Installing...");
    report_adb_result("INSTALLED", "INSTALL FAILED", rc, "Installed successfully.");
}

/* ---------------------------------------------------------
   File manager
   --------------------------------------------------------- */

static int is_apk_name(const char *name)
{
    size_t n = strlen(name);

    return n >= 4 && strcasecmp(name + n - 4, ".apk") == 0;
}

static int compare_entries(const void *a, const void *b)
{
    const Entry *x = a;
    const Entry *y = b;

    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;

    return strcasecmp(x->name, y->name);
}


/* Symlinks are followed for navigation, so a link to a folder opens it.
   The path shown is the path you navigated through (not the resolved one),
   so ".." goes back the way you came. Broken links appear only in
   "all files" mode and cannot be opened. Non-regular files (FIFOs, sockets,
   device nodes) also appear only in "all files" mode. */
static int load_directory(const char *path, int *truncated)
{
    DIR *dir = opendir(path);
    struct dirent *de;

    if (!dir)
        return -1;

    entry_count = 0;
    *truncated = 0;

    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        char full[PATH_SIZE];
        struct stat lst;
        struct stat st;
        int resolved;
        int is_dir;
        int is_reg;
        int is_apk;
        Entry *e;

        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;

        if (!show_hidden && name[0] == '.')
            continue;

        if (join_path(full, sizeof(full), path, name) != 0)
            continue;

        if (lstat(full, &lst) != 0)
            continue;

        resolved = (stat(full, &st) == 0);
        is_dir = resolved && S_ISDIR(st.st_mode);
        is_reg = resolved && S_ISREG(st.st_mode);
        is_apk = is_reg && is_apk_name(name);

        if (!resolved && !show_all_files)
            continue;

        if (!is_dir && !is_apk && !show_all_files)
            continue;

        if (entry_count >= MAX_ENTRIES) {
            *truncated = 1;
            break;
        }

        e = &entries[entry_count++];

        snprintf(e->name, sizeof(e->name), "%s", name);
        e->is_dir = is_dir;
        e->is_apk = is_apk;
        e->is_link = S_ISLNK(lst.st_mode);
        e->is_broken = !resolved;
        e->is_special = resolved && !is_dir && !is_reg;
        e->size = is_reg ? st.st_size : 0;
    }

    closedir(dir);

    qsort(entries, (size_t)entry_count, sizeof(Entry), compare_entries);
    return 0;
}

static void format_size(off_t bytes, char *out, size_t size)
{
    static const char *units[] = { "B", "KB", "MB", "GB" };
    double value = (double)bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0)
        snprintf(out, size, "%ld B", (long)bytes);
    else
        snprintf(out, size, "%.1f %s", value, units[unit]);
}

static void parent_path(char *path)
{
    if (strcmp(path, "/") == 0)
        return;

    char *slash = strrchr(path, '/');

    if (!slash || slash == path)
        strcpy(path, "/");
    else
        *slash = '\0';
}

static int join_path(char *dst, size_t size, const char *dir, const char *name)
{
    int n = (strcmp(dir, "/") == 0)
        ? snprintf(dst, size, "/%s", name)
        : snprintf(dst, size, "%s/%s", dir, name);

    return (n >= 0 && (size_t)n < size) ? 0 : -1;
}

/* Opens target, replacing path only on success. */
static int navigate(char *path, size_t size, const char *target,
                    const char *select_name,
                    int *selected, int *offset, int *truncated)
{
    char tmp[PATH_SIZE];

    snprintf(tmp, sizeof(tmp), "%s", target);

    if (load_directory(tmp, truncated) != 0)
        return -1;

    snprintf(path, size, "%s", tmp);

    *selected = 0;
    *offset = 0;

    if (select_name) {
        for (int i = 0; i < entry_count; i++) {
            if (!strcmp(entries[i].name, select_name)) {
                *selected = i;
                break;
            }
        }
    }

    return 0;
}

static void open_or_warn(char *path, size_t size, const char *target,
                         const char *select_name,
                         int *selected, int *offset, int *truncated)
{
    if (navigate(path, size, target, select_name, selected, offset, truncated) != 0)
        message_page("CANNOT OPEN", "This folder is not readable. Check storage permission.");
}

static void reload_keep_selection(char *path, size_t size,
                                  int *selected, int *offset, int *truncated)
{
    char keep[INPUT_SIZE] = "";

    if (*selected >= 0 && *selected < entry_count)
        snprintf(keep, sizeof(keep), "%s", entries[*selected].name);

    open_or_warn(path, size, path, keep, selected, offset, truncated);
}

static int list_height(int rows)
{
    int h = rows - LIST_TOP - 6;

    return h < 1 ? 1 : h;
}


static void draw_browser(const char *path, int selected, int offset, int truncated)
{
    int rows;
    int cols;
    int width;
    int list_h;
    char shown[PATH_SIZE];
    char status[160];

    getmaxyx(stdscr, rows, cols);
    header(NULL);

    if (screen_too_small(rows, cols)) {
        centered(rows / 2, "Terminal too small - resize it, or press q.", CP_ACCENT);
        refresh();
        return;
    }

    width = cols - 4;

    /* Keep the tail of long paths: that is where you are. */
    if ((int)strlen(path) > width)
        snprintf(shown, sizeof(shown), "...%s", path + strlen(path) - (size_t)(width - 3));
    else
        snprintf(shown, sizeof(shown), "%s", path);

    color_on(CP_ACCENT);
    mvaddnstr(6, 2, shown, width);
    color_off(CP_ACCENT);

    snprintf(status, sizeof(status),
             "%d items | hidden: %s | all files: %s%s",
             entry_count,
             show_hidden ? "on" : "off",
             show_all_files ? "on" : "off",
             truncated ? " | LIST LIMITED" : "");

    color_on(CP_MUTED);
    mvaddnstr(7, 2, status, width);
    color_off(CP_MUTED);

    list_h = list_height(rows);

    if (entry_count == 0)
        centered(LIST_TOP + 1, "This folder has no visible items", CP_MUTED);

    for (int i = 0; i < list_h; i++) {
        int index = offset + i;
        int y = LIST_TOP + i;
        Entry *e;
        int is_sel;
        int pair;
        char size_text[32] = "";
        char label[PATH_SIZE];
        const char *prefix;
        int size_len;
        int name_width;

        if (index >= entry_count)
            break;

        e = &entries[index];
        is_sel = (index == selected);
        pair = is_sel ? CP_SELECTED : CP_NORMAL;

        if (e->is_broken)
            prefix = "[BRK] ";
        else if (e->is_dir)
            prefix = "[DIR] ";
        else if (e->is_apk)
            prefix = "[APK] ";
        else if (e->is_special)
            prefix = "[---] ";
        else
            prefix = "      ";

        if (!e->is_dir && !e->is_broken && !e->is_special)
            format_size(e->size, size_text, sizeof(size_text));

        snprintf(label, sizeof(label), "%s%s%s%s",
                 prefix, e->name,
                 e->is_dir ? "/" : "",
                 e->is_link ? " @" : "");

        size_len = (int)strlen(size_text);
        name_width = width - size_len - 2;

        if (name_width < 1)
            name_width = 1;

        color_on(pair);
        mvhline(y, 2, ' ', width);
        mvaddnstr(y, 2, label, name_width);
        color_off(pair);

        if (size_len > 0) {
            int size_pair = is_sel ? CP_SELECTED : CP_MUTED;

            color_on(size_pair);
            mvaddstr(y, 2 + width - size_len, size_text);
            color_off(size_pair);
        }
    }

    footer_lines("ENTER open/install  BKSP up  g go to path  h home",
                 ". hidden  a all files  r refresh  q back");
    refresh();
}

static void file_manager(void)
{
    char path[PATH_SIZE];
    int selected = 0;
    int offset = 0;
    int truncated = 0;

    if (navigate(path, sizeof(path), START_DIR, NULL,
                 &selected, &offset, &truncated) != 0) {
        if (navigate(path, sizeof(path), home_dir, NULL,
                     &selected, &offset, &truncated) != 0)
            return;
    }

    while (1) {
        int rows, cols;

        getmaxyx(stdscr, rows, cols);
        (void)cols;

        int list_h = list_height(rows);
        int page = list_h > 1 ? list_h - 1 : 1;

        if (entry_count == 0)
            selected = 0;
        else if (selected >= entry_count)
            selected = entry_count - 1;

        if (selected < offset)
            offset = selected;

        if (selected >= offset + list_h)
            offset = selected - list_h + 1;

        if (offset < 0)
            offset = 0;

        draw_browser(path, selected, offset, truncated);

        int key = getch();

        switch (key) {
        case KEY_UP:
            if (selected > 0)
                selected--;
            break;

        case KEY_DOWN:
            if (selected + 1 < entry_count)
                selected++;
            break;

        case KEY_PPAGE:
            selected = (selected - page < 0) ? 0 : selected - page;
            break;

        case KEY_NPAGE:
            if (entry_count > 0)
                selected = (selected + page >= entry_count) ? entry_count - 1 : selected + page;
            break;

        case KEY_HOME:
            selected = 0;
            break;

        case KEY_END:
            if (entry_count > 0)
                selected = entry_count - 1;
            break;

        case '\n':
        case KEY_ENTER:
        case KEY_RIGHT: {
            if (selected < 0 || selected >= entry_count)
                break;

            Entry *e = &entries[selected];
            char target[PATH_SIZE];

            if (join_path(target, sizeof(target), path, e->name) != 0) {
                message_page("PATH TOO LONG", "This path is too long to open.");
                break;
            }

            if (e->is_broken) {
                message_page("BROKEN LINK", "This symbolic link points to nothing.");
            } else if (e->is_dir) {
                open_or_warn(path, sizeof(path), target, NULL,
                             &selected, &offset, &truncated);
            } else if (e->is_apk) {
                install_apk(target);
            } else if (e->is_special) {
                message_page("NOT A REGULAR FILE", "Devices, pipes and sockets cannot be opened here.");
            } else {
                message_page("NOT AN APK", "Only .apk files can be installed.");
            }
            break;
        }

        case KEY_LEFT:
        case KEY_BACKSPACE:
        case 127:
        case 8:
        case 'u':
        case 'U': {
            if (strcmp(path, "/") == 0)
                break;

            char child[INPUT_SIZE];
            char parent[PATH_SIZE];

            snprintf(child, sizeof(child), "%s", basename_of(path));
            snprintf(parent, sizeof(parent), "%s", path);
            parent_path(parent);

            open_or_warn(path, sizeof(path), parent, child,
                     &selected, &offset, &truncated);
            break;
        }

        case 'h':
        case 'H':
            open_or_warn(path, sizeof(path), home_dir, NULL,
                     &selected, &offset, &truncated);
            break;

        case 'g':
        case 'G': {
            char input[PATH_SIZE];

            if (read_field("Path (e.g. /storage/emulated/0/Download, ~ = home):",
                           input, sizeof(input)) && input[0]) {
                char target[PATH_SIZE];

                if (input[0] == '~') {
                    snprintf(target, sizeof(target), "%s", home_dir);
                    strncat(target, input + 1, sizeof(target) - strlen(target) - 1);
                } else {
                    snprintf(target, sizeof(target), "%s", input);
                }

                if (target[0] != '/')
                    message_page("INVALID PATH", "Enter an absolute path.");
                else
                    open_or_warn(path, sizeof(path), target, NULL,
                             &selected, &offset, &truncated);
            }
            break;
        }

        case '.':
            show_hidden = !show_hidden;
            reload_keep_selection(path, sizeof(path), &selected, &offset, &truncated);
            break;

        case 'a':
        case 'A':
            show_all_files = !show_all_files;
            reload_keep_selection(path, sizeof(path), &selected, &offset, &truncated);
            break;

        case 'r':
        case 'R':
            reload_keep_selection(path, sizeof(path), &selected, &offset, &truncated);
            break;

        case 'q':
        case 'Q':
        case 27:
            return;

        case KEY_RESIZE:
            clear();
            break;

        default:
            break;
        }
    }
}

/* ---------------------------------------------------------
   Main
   --------------------------------------------------------- */

int main(void)
{
    setlocale(LC_ALL, "");

    const char *home = getenv("HOME");

    if (!home || !*home) {
        fprintf(stderr, "HOME is not set.\n");
        return 1;
    }

    snprintf(home_dir, sizeof(home_dir), "%s", home);

    if (snprintf(config_dir, sizeof(config_dir), "%s/.config/cli-ins", home)
        >= (int)sizeof(config_dir) ||
        snprintf(phonebook_path, sizeof(phonebook_path), "%s/devices.tsv", config_dir)
        >= (int)sizeof(phonebook_path) ||
        snprintf(theme_path, sizeof(theme_path), "%s/theme.conf", config_dir)
        >= (int)sizeof(theme_path)) {
        fprintf(stderr, "Configuration path too long.\n");
        return 1;
    }

    if (ensure_config_dir(home) != 0) {
        fprintf(stderr, "Could not create CLI-INS configuration directory.\n");
        return 1;
    }

    load_phonebook();

    setenv("ESCDELAY", "25", 0);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors() && start_color() == OK) {
        use_colors = 1;
        default_bg_ok = (use_default_colors() == OK);
    }

    load_theme();

    const char *items[] = {
        "Pair a device",
        "Connect to a device",
        "File manager",
        "Theme",
        "Exit"
    };

    int running = 1;

    while (running) {
        int choice = menu("MAIN MENU", items, 5);

        switch (choice) {
        case 0:
            pair_device();
            break;

        case 1:
            if (check_adb())
                saved_device_list();
            break;

        case 2:
            file_manager();
            break;

        case 3:
            theme_menu();
            break;

        case MENU_CANCEL:
        default:
            running = 0;
            break;
        }
    }

    endwin();

    printf("%s %s\n", APP_NAME, APP_VERSION);
    printf("Thanks for using CLI-INS!\n");

    return 0;
}

