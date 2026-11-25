#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
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
    off_t size;
} Entry;

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

static int menu(const char *title, const char *items[], int count);

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

static void save_theme(void)
{
    FILE *fp = fopen(theme_path, "w");

    if (!fp)
        return;

    fprintf(fp, "%s\n%d %d %d %d %d %d %d\n",
            current_theme.name,
            current_theme.fg, current_theme.bg, current_theme.accent,
            current_theme.sel_fg, current_theme.sel_bg,
            current_theme.in_fg, current_theme.in_bg);

    fclose(fp);
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
    int text, bg, accent;

    for (int i = 0; i < 8; i++)
        items[i] = color_names[i];

    items[8] = "Cancel";

    text = menu("CUSTOM: TEXT COLOR", items, 9);
    if (text < 0 || text >= 8)
        return;

    bg = menu("CUSTOM: BACKGROUND", items, 9);
    if (bg < 0 || bg >= 8)
        return;

    accent = menu("CUSTOM: ACCENT COLOR", items, 9);
    if (accent < 0 || accent >= 8)
        return;

    Theme t = {
        "Custom",
        (short)text, (short)bg, (short)accent,
        COLOR_BLACK, (short)accent,
        COLOR_WHITE, COLOR_BLUE
    };

    apply_theme(&t);
    save_theme();
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
            apply_theme(&t);
            save_theme();
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

static int valid_host(const char *host)
{
    if (!host || !*host || strlen(host) >= INPUT_SIZE)
        return 0;

    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (!(isalnum(c) || c == '.' || c == ':' || c == '-' || c == '_'))
            return 0;
    }

    return 1;
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
    if (!code || strlen(code) != 6)
        return 0;

    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)code[i]))
            return 0;
    }

    return 1;
}

static int safe_name(const char *name)
{
    if (!name || !*name)
        return 0;

    for (const char *p = name; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '|')
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
    char temporary[PATH_SIZE];

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", phonebook_path)
        >= (int)sizeof(temporary))
        return -1;

    int fd = mkstemp(temporary);

    if (fd < 0)
        return -1;

    if (fchmod(fd, 0600) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }

    FILE *fp = fdopen(fd, "w");

    if (!fp) {
        close(fd);
        unlink(temporary);
        return -1;
    }

    int failed = 0;

    for (int i = 0; i < device_count; i++) {
        if (fprintf(fp, "%s\t%s\n", devices[i].name, devices[i].host) < 0) {
            failed = 1;
            break;
        }
    }

    if (fflush(fp) != 0)
        failed = 1;

    if (fsync(fd) != 0)
        failed = 1;

    if (fclose(fp) != 0)
        failed = 1;

    if (failed || rename(temporary, phonebook_path) != 0) {
        unlink(temporary);
        return -1;
    }

    return 0;
}

static void load_phonebook(void)
{
    device_count = 0;

    FILE *fp = fopen(phonebook_path, "r");

    if (!fp)
        return;

    char line[INPUT_SIZE * 3 + 64];

    while (fgets(line, sizeof(line), fp) && device_count < MAX_DEVICES) {
        line[strcspn(line, "\r\n")] = '\0';

        char *name = strtok(line, "\t");
        char *host = strtok(NULL, "\t");

        if (!name || !host || !safe_name(name) || !valid_host(host))
            continue;

        snprintf(devices[device_count].name, sizeof(devices[device_count].name), "%s", name);
        snprintf(devices[device_count].host, sizeof(devices[device_count].host), "%s", host);
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
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    header("INPUT");

    if (rows > 8) {
        color_on(CP_TITLE);
        mvaddnstr(8, 4, label, cols - 8);
        color_off(CP_TITLE);
    }

    if (rows > 12) {
        color_on(CP_INPUT);
        mvaddstr(10, 4, "> ");
        color_off(CP_INPUT);
    }

    buffer[0] = '\0';

    echo();
    curs_set(1);
    move(10, 6);
    clrtoeol();

    int result = getnstr(buffer, (int)size - 1);

    noecho();
    curs_set(0);

    return result != ERR;
}

static int read_secret_field(const char *label, char *buffer, size_t size)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
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

    size_t used = 0;

    curs_set(1);
    move(10, 6);
    refresh();

    while (1) {
        int ch = getch();

        if (ch == '\n' || ch == KEY_ENTER)
            break;

        if (ch == 27) {
            buffer[0] = '\0';
            curs_set(0);
            return 0;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (used > 0) {
                used--;
                buffer[used] = '\0';
                addch('\b');
                addch(' ');
                addch('\b');
                refresh();
            }
            continue;
        }

        if (ch >= '0' && ch <= '9' && used + 1 < size) {
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

static int run_command(char *const argv[])
{
    fflush(NULL);
    endwin();

    printf("\n$");

    for (int i = 0; argv[i]; i++) {
        if (i == 3 && strcmp(argv[0], "adb") == 0 && strcmp(argv[1], "pair") == 0)
            printf(" [HIDDEN]");
        else
            printf(" %s", argv[i]);
    }

    printf("\n\n");
    fflush(stdout);

    pid_t pid = fork();

    if (pid == 0) {
        execvp(argv[0], argv);
        perror("Could not start command");
        _exit(127);
    }

    int result = -1;

    if (pid > 0)
        result = wait_for_child(pid);
    else
        perror("fork");

    if (result >= 0)
        printf("\nCommand exit code: %d\n", result);
    else
        printf("\nCommand did not finish normally.\n");

    printf("\nPress Enter to return...");
    fflush(stdout);

    int ch;

    do {
        ch = getchar();
    } while (ch != '\n' && ch != EOF);

    reset_prog_mode();
    clear();
    refresh();

    return result;
}

static int capture_command(char *const argv[], char *output, size_t output_size)
{
    if (!output || output_size < 2)
        return -1;

    output[0] = '\0';

    int pipefd[2];

    if (pipe(pipefd) != 0)
        return -1;

    pid_t pid = fork();

    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);

        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    size_t used = 0;
    char buffer[4096];

    while (1) {
        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (n == 0)
            break;

        size_t copy = (size_t)n;

        if (copy > output_size - used - 1)
            copy = output_size - used - 1;

        if (copy > 0) {
            memcpy(output + used, buffer, copy);
            used += copy;
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

static int check_adb(void)
{
    if (!have_command("adb")) {
        message_page("ADB NOT FOUND", "Install android-tools using pkg first.");
        return 0;
    }

    return 1;
}

/* ---------------------------------------------------------
   ADB devices
   --------------------------------------------------------- */

static int get_active_devices(char serials[][SERIAL_SIZE], int max_devices)
{
    char *output = malloc(OUTPUT_SIZE);

    if (!output)
        return -1;

    char *args[] = { "adb", "devices", NULL };

    if (capture_command(args, output, OUTPUT_SIZE) != 0) {
        free(output);
        return -1;
    }

    int count = 0;
    char *save = NULL;
    char *line = strtok_r(output, "\n", &save);

    while (line && count < max_devices) {
        char listed[SERIAL_SIZE];
        char state[128];

        if (sscanf(line, "%511s %127s", listed, state) == 2 && strcmp(state, "device") == 0) {
            snprintf(serials[count], SERIAL_SIZE, "%s", listed);
            count++;
        }

        line = strtok_r(NULL, "\n", &save);
    }

    free(output);
    return count;
}

static int find_active_serial(const char *host, char *serial, size_t serial_size)
{
    char (*serials)[SERIAL_SIZE] = malloc(sizeof(char[SERIAL_SIZE]) * MAX_DEVICES);

    if (!serials)
        return 0;

    int count = get_active_devices(serials, MAX_DEVICES);
    int found = 0;
    size_t host_len = strlen(host);

    for (int i = 0; i < count && !found; i++) {
        if (strncmp(serials[i], host, host_len) == 0 && serials[i][host_len] == ':') {
            snprintf(serial, serial_size, "%s", serials[i]);
            found = 1;
        }
    }

    free(serials);
    return found;
}

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
    char code[16];
    char address[INPUT_SIZE + 20];

    if (!check_adb())
        return;

    if (!read_field("Friendly device name:", name, sizeof(name)) ||
        !read_field("Device IP address:", host, sizeof(host)) ||
        !read_field("Pairing port:", port, sizeof(port)) ||
        !read_secret_field("Six-digit pairing code:", code, sizeof(code)))
        return;

    if (!safe_name(name) || !valid_host(host) || !valid_port(port) || !valid_code(code)) {
        secure_wipe(code, sizeof(code));
        message_page("INVALID INPUT", "Check the name, IP, port and six-digit code.");
        return;
    }

    snprintf(address, sizeof(address), "%s:%s", host, port);

    char *args[] = { "adb", "pair", address, code, NULL };

    int result = run_command(args);

    secure_wipe(code, sizeof(code));

    if (result == 0) {
        if (save_device(name, host) == 0)
            message_page("DEVICE PAIRED", "Device name and IP saved successfully.");
        else
            message_page("SAVE ERROR", "Pairing succeeded, but saving failed.");
    } else {
        message_page("PAIRING FAILED", "Check the pairing code, port and IP.");
    }
}

static void connect_saved_device(int index)
{
    if (index < 0 || index >= device_count)
        return;

    Device d = devices[index];
    char serial[SERIAL_SIZE];

    if (find_active_serial(d.host, serial, sizeof(serial))) {
        if (!confirm_disconnect(d.name))
            return;

        char *args[] = { "adb", "disconnect", serial, NULL };
        int result = run_command(args);

        if (result == 0)
            message_page("DISCONNECTED", "ADB disconnect command completed.");
        else
            message_page("DISCONNECT FAILED", "ADB could not disconnect this device.");

        return;
    }

    char port[16];
    char address[INPUT_SIZE + 20];

    if (!read_field("Wireless debugging connection port:", port, sizeof(port)))
        return;

    if (!valid_port(port)) {
        message_page("INVALID PORT", "Enter a valid connection port.");
        return;
    }

    snprintf(address, sizeof(address), "%s:%s", d.host, port);

    char *args[] = { "adb", "connect", address, NULL };
    int result = run_command(args);

    if (result == 0)
        message_page("CONNECTION COMMAND SENT", "The device list will refresh.");
    else
        message_page("CONNECTION FAILED", "Check the IP, port and wireless debugging.");
}

static void saved_device_list(void)
{
    while (1) {
        load_phonebook();

        if (device_count == 0) {
            message_page("SAVED DEVICES", "No saved devices. Pair a device first.");
            return;
        }

        char labels[MAX_DEVICES][INPUT_SIZE + 64];
        const char *items[MAX_DEVICES + 2];
        char serial[SERIAL_SIZE];

        for (int i = 0; i < device_count; i++) {
            if (find_active_serial(devices[i].host, serial, sizeof(serial)))
                snprintf(labels[i], sizeof(labels[i]), "%s: DISCONNECT", devices[i].name);
            else
                snprintf(labels[i], sizeof(labels[i]), "%s: CONNECT", devices[i].name);

            items[i] = labels[i];
        }

        items[device_count] = "Remove a saved device";
        items[device_count + 1] = "Back";

        int selected = menu("SAVED DEVICES", items, device_count + 2);

        if (selected < 0 || selected == device_count + 1)
            return;

        if (selected == device_count) {
            const char *remove_items[MAX_DEVICES + 1];

            for (int i = 0; i < device_count; i++)
                remove_items[i] = devices[i].name;

            remove_items[device_count] = "Cancel";

            int target = menu("REMOVE SAVED DEVICE", remove_items, device_count + 1);

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

static int choose_adb_device(char serials[][SERIAL_SIZE], int count)
{
    if (count <= 0)
        return -1;

    char labels[MAX_DEVICES][INPUT_SIZE + SERIAL_SIZE + 8];
    const char *items[MAX_DEVICES + 1];

    for (int i = 0; i < count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%s | %s",
                 friendly_name(serials[i]), serials[i]);
        items[i] = labels[i];
    }

    items[count] = "Cancel";

    int selected = menu("CHOOSE INSTALLATION TARGET", items, count + 1);

    if (selected < 0 || selected >= count)
        return -1;

    return selected;
}

static void install_apk(const char *apk_path)
{
    struct stat st;

    if (stat(apk_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        message_page("INVALID APK", "The selected file is unavailable.");
        return;
    }

    if (!check_adb())
        return;

    char (*serials)[SERIAL_SIZE] = malloc(sizeof(char[SERIAL_SIZE]) * MAX_DEVICES);

    if (!serials) {
        message_page("MEMORY ERROR", "Could not allocate device list.");
        return;
    }

    int count = get_active_devices(serials, MAX_DEVICES);

    if (count < 0) {
        free(serials);
        message_page("ADB ERROR", "Could not read the ADB device list.");
        return;
    }

    if (count == 0) {
        free(serials);
        message_page("NO CONNECTED DEVICES", "Connect an ADB device before installing.");
        return;
    }

    int selected = choose_adb_device(serials, count);

    if (selected < 0) {
        free(serials);
        return;
    }

    char prompt[INPUT_SIZE + 80];

    snprintf(prompt, sizeof(prompt), "Install %.150s?", basename_of(apk_path));

    if (!confirm_action(prompt, "Install")) {
        free(serials);
        return;
    }

    char *args[] = {
        "adb", "-s", serials[selected], "install", "-r", (char *)apk_path, NULL
    };

    int result = run_command(args);

    free(serials);

    if (result == 0)
        message_page("INSTALLATION FINISHED", "ADB completed the installation command.");
    else
        message_page("INSTALLATION FAILED", "Check ADB output; the existing app was kept.");
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

static int load_directory(const char *path, int *truncated)
{
    DIR *dir = opendir(path);

    if (!dir)
        return -1;

    entry_count = 0;
    *truncated = 0;

    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;

        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;

        if (!show_hidden && name[0] == '.')
            continue;

        char full[PATH_SIZE];

        if (snprintf(full, sizeof(full), "%s/%s", path, name) >= (int)sizeof(full))
            continue;

        struct stat st;

        if (stat(full, &st) != 0)
            continue;

        int is_dir = S_ISDIR(st.st_mode);
        int is_reg = S_ISREG(st.st_mode);
        int is_apk = is_reg && is_apk_name(name);

        if (!is_dir && !is_apk && !show_all_files)
            continue;

        if (entry_count >= MAX_ENTRIES) {
            *truncated = 1;
            break;
        }

        Entry *e = &entries[entry_count++];

        snprintf(e->name, sizeof(e->name), "%s", name);
        e->is_dir = is_dir;
        e->is_apk = is_apk;
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
    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    header(NULL);

    int width = cols - 4;
    char shown[PATH_SIZE];

    snprintf(shown, sizeof(shown), "%s", path);

    if (width > 4 && (int)strlen(path) > width)
        snprintf(shown, sizeof(shown), "...%s", path + strlen(path) - (width - 3));

    color_on(CP_ACCENT);
    mvaddnstr(6, 2, shown, width);
    color_off(CP_ACCENT);

    char status[160];

    snprintf(status, sizeof(status),
             "%d items | hidden: %s | all files: %s%s",
             entry_count,
             show_hidden ? "on" : "off",
             show_all_files ? "on" : "off",
             truncated ? " | LIST LIMITED" : "");

    color_on(CP_MUTED);
    mvaddnstr(7, 2, status, width);
    color_off(CP_MUTED);

    int list_h = list_height(rows);

    if (entry_count == 0) {
        centered(LIST_TOP + 1, "This folder has no visible items", CP_MUTED);
    }

    for (int i = 0; i < list_h; i++) {
        int index = offset + i;
        int y = LIST_TOP + i;

        if (index >= entry_count)
            break;

        Entry *e = &entries[index];
        int is_sel = (index == selected);
        int pair = is_sel ? CP_SELECTED : CP_NORMAL;

        char size_text[32] = "";

        if (!e->is_dir)
            format_size(e->size, size_text, sizeof(size_text));

        char label[PATH_SIZE];

        snprintf(label, sizeof(label), "%s%s%s",
                 e->is_dir ? "[DIR] " : (e->is_apk ? "[APK] " : "      "),
                 e->name,
                 e->is_dir ? "/" : "");

        int size_len = (int)strlen(size_text);
        int name_width = width - size_len - 2;

        if (name_width < 1)
            name_width = 1;

        color_on(pair);
        mvhline(y, 2, ' ', width);
        mvaddnstr(y, 2, label, name_width);
        color_off(pair);

        if (size_len > 0) {
            color_on(is_sel ? CP_SELECTED : CP_MUTED);
            mvaddstr(y, 2 + width - size_len, size_text);
            color_off(is_sel ? CP_SELECTED : CP_MUTED);
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

            if (e->is_dir) {
                open_or_warn(path, sizeof(path), target, NULL,
                         &selected, &offset, &truncated);
            } else if (e->is_apk) {
                install_apk(target);
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

