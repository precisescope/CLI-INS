#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <dirent.h>
#include <locale.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define APP_NAME "CLI-INS"
#define APP_VERSION "5.0"

#define MAX_ITEMS 512
#define MAX_DEVICES 256
#define PATH_SIZE 4096
#define INPUT_SIZE 256
#define SERIAL_SIZE 512
#define CAPTURE_SIZE 1048576

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
    const char *name;
    const char *package;
    const char *filename;
    const char *source;
    const char *api_url;
    const char *jq_filter;
} RecommendedApp;

static Device devices[MAX_DEVICES];
static int device_count;

static char config_dir[PATH_SIZE];
static char phonebook_path[PATH_SIZE];
static char apk_dir[PATH_SIZE];

static int use_colors;

/* ---------------------------------------------------------
   Recommended applications
   --------------------------------------------------------- */

static const RecommendedApp recommended_apps[] = {
    {
        "Termux:X11",
        "com.termux.x11",
        "termux-x11-universal-debug.apk",
        "Termux:X11 official GitHub nightly release",
        "https://api.github.com/repos/termux/termux-x11/releases/tags/nightly",
        ".assets[] | select(.name == \"termux-x11-universal-debug.apk\") | .browser_download_url"
    },
    {
        "InstallerX Revived",
        "com.rosan.installer.x.revived",
        "installerx-revived.apk",
        "InstallerX Revived official GitHub releases",
        "https://api.github.com/repos/wxxsfxyzm/InstallerX-Revived/releases/latest",
        "[.assets[] | select(.name | endswith(\".apk\"))][0].browser_download_url // empty"
    },
    {
        "F-Droid",
        "org.fdroid.fdroid",
        "fdroid.apk",
        "F-Droid official website",
        NULL,
        NULL
    }
};

#define RECOMMENDED_COUNT \
    ((int)(sizeof(recommended_apps) / sizeof(recommended_apps[0])))

/* ---------------------------------------------------------
   Configuration and paths
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

static int ensure_directory_tree(void)
{
    char path[PATH_SIZE];
    const char *home = getenv("HOME");

    if (!home || !*home)
        return -1;

    if (snprintf(path, sizeof(path), "%s/.config", home)
        >= (int)sizeof(path))
        return -1;

    if (make_directory(path) != 0)
        return -1;

    if (make_directory(config_dir) != 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/.local", home)
        >= (int)sizeof(path))
        return -1;

    if (make_directory(path) != 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/.local/share", home)
        >= (int)sizeof(path))
        return -1;

    if (make_directory(path) != 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/.local/share/cli-ins", home)
        >= (int)sizeof(path))
        return -1;

    if (make_directory(path) != 0)
        return -1;

    if (make_directory(apk_dir) != 0)
        return -1;

    return 0;
}

static int valid_host(const char *host)
{
    if (!host || !*host || strlen(host) >= INPUT_SIZE)
        return 0;

    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (!(isalnum(c) || c == '.' || c == ':' ||
              c == '-' || c == '_'))
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

    return errno == 0 && end && !*end &&
           value >= 1 && value <= 65535;
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
        if (*p == '\t' || *p == '\n' ||
            *p == '\r' || *p == '|')
            return 0;
    }

    return 1;
}

/* ---------------------------------------------------------
   Phonebook
   --------------------------------------------------------- */

static int save_phonebook(void)
{
    char temporary[PATH_SIZE];

    if (snprintf(temporary, sizeof(temporary),
                 "%s.tmp.XXXXXX", phonebook_path)
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
        if (fprintf(fp, "%s\t%s\n",
                    devices[i].name,
                    devices[i].host) < 0) {
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

    while (fgets(line, sizeof(line), fp) &&
           device_count < MAX_DEVICES) {
        line[strcspn(line, "\r\n")] = '\0';

        char *name = strtok(line, "\t");
        char *host = strtok(NULL, "\t");

        if (!name || !host ||
            !safe_name(name) || !valid_host(host))
            continue;

        snprintf(devices[device_count].name,
                 sizeof(devices[device_count].name),
                 "%s", name);

        snprintf(devices[device_count].host,
                 sizeof(devices[device_count].host),
                 "%s", host);

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

            snprintf(devices[i].name,
                     sizeof(devices[i].name), "%s", name);

            if (save_phonebook() != 0) {
                devices[i] = old;
                return -1;
            }

            return 0;
        }
    }

    if (device_count >= MAX_DEVICES)
        return -1;

    snprintf(devices[device_count].name,
             sizeof(devices[device_count].name),
             "%s", name);

    snprintf(devices[device_count].host,
             sizeof(devices[device_count].host),
             "%s", host);

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

    Device backup[MAX_DEVICES];
    int old_count = device_count;

    memcpy(backup, devices, sizeof(devices));

    for (int i = index; i < device_count - 1; i++)
        devices[i] = devices[i + 1];

    device_count--;

    if (save_phonebook() != 0) {
        memcpy(devices, backup, sizeof(devices));
        device_count = old_count;
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------
   Theme and drawing
   --------------------------------------------------------- */

static void init_theme(void)
{
    if (!has_colors())
        return;

    start_color();
    use_default_colors();

    init_pair(CP_NORMAL, COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_TITLE, COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_SELECTED, COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_MUTED, COLOR_CYAN, COLOR_BLACK);
    init_pair(CP_ACCENT, COLOR_CYAN, COLOR_BLACK);
    init_pair(CP_BORDER, COLOR_CYAN, COLOR_BLACK);
    init_pair(CP_INPUT, COLOR_WHITE, COLOR_BLUE);

    bkgd(COLOR_PAIR(CP_NORMAL));
    use_colors = 1;
}

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

static void footer(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows < 6)
        return;

    centered(rows - 4,
             "ARROWS: Navigate   ENTER: Select   ESC: Back",
             CP_MUTED);

    if (cols >= 60) {
        centered(rows - 3,
                 "Made by precisescope (GitHub) | 8bit_binary (Instagram)",
                 CP_MUTED);
    } else {
        centered(rows - 3, "GitHub: precisescope", CP_MUTED);
    }
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

static int read_field(const char *label,
                      char *buffer, size_t size)
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

static int read_secret_field(const char *label,
                             char *buffer, size_t size)
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

static int menu(const char *title,
                const char *items[], int count)
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
            centered(rows / 2,
                     "Terminal too small - resize it.",
                     CP_ACCENT);
            refresh();

            int key = getch();

            if (key == KEY_RESIZE)
                continue;

            if (key == 27)
                return count - 1;

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

        draw_box(panel_y, panel_x,
                 panel_height, panel_width);

        color_on(CP_MUTED);
        mvaddnstr(panel_y + 1, panel_x + 3,
                  "SELECT AN OPTION", panel_width - 6);
        color_off(CP_MUTED);

        for (int i = 0; i < visible; i++) {
            int index = offset + i;
            int y = panel_y + 2 + i;

            if (index >= count)
                break;

            if (index == selected) {
                color_on(CP_SELECTED);
                mvhline(y, panel_x + 1, ' ',
                        panel_width - 2);
                mvaddnstr(y, panel_x + 3, "> ", 2);
                mvaddnstr(y, panel_x + 5,
                          items[index], panel_width - 8);
                color_off(CP_SELECTED);
            } else {
                color_on(CP_NORMAL);
                mvaddnstr(y, panel_x + 3, "  ", 2);
                mvaddnstr(y, panel_x + 5,
                          items[index], panel_width - 8);
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
            return count - 1;

        case KEY_RESIZE:
            clear();
            break;

        default:
            if (key == 'q' || key == 'Q')
                return count - 1;
            break;
        }
    }
}

/* ---------------------------------------------------------
   External command execution
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
        if (i == 3 &&
            strcmp(argv[0], "adb") == 0 &&
            strcmp(argv[1], "pair") == 0) {
            printf(" [HIDDEN]");
        } else {
            printf(" %s", argv[i]);
        }
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

    if (pid > 0) {
        result = wait_for_child(pid);
    } else {
        perror("fork");
    }

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

static int capture_command(char *const argv[],
                           char *output, size_t output_size)
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

static int capture_input(char *const argv[],
                         const char *input,
                         char *output, size_t output_size)
{
    int inpipe[2];
    int outpipe[2];

    if (pipe(inpipe) != 0)
        return -1;

    if (pipe(outpipe) != 0) {
        close(inpipe[0]);
        close(inpipe[1]);
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(inpipe[1]);
        close(outpipe[0]);

        if (dup2(inpipe[0], STDIN_FILENO) < 0)
            _exit(126);

        if (dup2(outpipe[1], STDOUT_FILENO) < 0)
            _exit(126);

        close(inpipe[0]);
        close(outpipe[1]);

        execvp(argv[0], argv);
        _exit(127);
    }

    close(inpipe[0]);
    close(outpipe[1]);

    size_t input_len = strlen(input);
    size_t sent = 0;

    while (sent < input_len) {
        ssize_t n = write(inpipe[1],
                          input + sent,
                          input_len - sent);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        sent += (size_t)n;
    }

    close(inpipe[1]);

    size_t used = 0;
    char buffer[4096];

    while (1) {
        ssize_t n = read(outpipe[0], buffer, sizeof(buffer));

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

    close(outpipe[0]);

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

        int n = snprintf(candidate, sizeof(candidate),
                         "%s/%s", token, command);

        if (n >= 0 && (size_t)n < sizeof(candidate) &&
            access(candidate, X_OK) == 0) {
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
        message_page("ADB NOT FOUND",
                     "Install android-tools using pkg first.");
        return 0;
    }

    return 1;
}

static int check_download_tools(void)
{
    if (!have_command("curl") || !have_command("jq")) {
        message_page("DOWNLOAD TOOLS MISSING",
                     "Install curl and jq using pkg first.");
        return 0;
    }

    return 1;
}

/* ---------------------------------------------------------
   ADB device discovery
   --------------------------------------------------------- */

static int get_active_devices(char serials[][SERIAL_SIZE],
                              int max_devices)
{
    char output[65536];

    char *args[] = {
        "adb", "devices", NULL
    };

    if (capture_command(args, output, sizeof(output)) != 0)
        return -1;

    int count = 0;
    char *save = NULL;
    char *line = strtok_r(output, "\n", &save);

    while (line && count < max_devices) {
        char listed[SERIAL_SIZE];
        char state[128];

        if (sscanf(line, "%511s %127s", listed, state) == 2 &&
            strcmp(state, "device") == 0) {
            snprintf(serials[count],
                     SERIAL_SIZE, "%s", listed);
            count++;
        }

        line = strtok_r(NULL, "\n", &save);
    }

    return count;
}

static int find_active_serial(const char *host,
                              char *serial,
                              size_t serial_size)
{
    char serials[MAX_DEVICES][SERIAL_SIZE];

    int count = get_active_devices(serials, MAX_DEVICES);

    if (count <= 0)
        return 0;

    size_t host_len = strlen(host);

    for (int i = 0; i < count; i++) {
        if (strncmp(serials[i], host, host_len) == 0 &&
            serials[i][host_len] == ':') {
            snprintf(serial, serial_size, "%s", serials[i]);
            return 1;
        }
    }

    return 0;
}

static const char *friendly_name(const char *serial)
{
    static char result[INPUT_SIZE];

    for (int i = 0; i < device_count; i++) {
        size_t len = strlen(devices[i].host);

        if (strncmp(serial, devices[i].host, len) == 0 &&
            serial[len] == ':') {
            snprintf(result, sizeof(result), "%s",
                     devices[i].name);
            return result;
        }
    }

    snprintf(result, sizeof(result), "%s", serial);
    return result;
}

static int confirm_disconnect(const char *name)
{
    char prompt[INPUT_SIZE + 64];

    snprintf(prompt, sizeof(prompt),
             "Disconnect from %.150s?", name);

    const char *items[] = {
        "No",
        "Yes, disconnect"
    };

    return menu(prompt, items, 2) == 1;
}

/* ---------------------------------------------------------
   Wireless ADB pairing
   --------------------------------------------------------- */

static void pair_device(void)
{
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
    char port[16];
    char code[INPUT_SIZE];
    char address[INPUT_SIZE + 20];

    if (!check_adb())
        return;

    if (!read_field("Friendly device name:",
                    name, sizeof(name)) ||
        !read_field("Device IP address:",
                    host, sizeof(host)) ||
        !read_field("Pairing port:",
                    port, sizeof(port)) ||
        !read_secret_field("Six-digit pairing code:",
                           code, sizeof(code)))
        return;

    if (!safe_name(name) ||
        !valid_host(host) ||
        !valid_port(port) ||
        !valid_code(code)) {
        message_page("INVALID INPUT",
                     "Check the name, IP, port and six-digit code.");
        return;
    }

    snprintf(address, sizeof(address), "%s:%s", host, port);

    char *args[] = {
        "adb", "pair", address, code, NULL
    };

    int result = run_command(args);

    memset(code, 0, sizeof(code));

    if (result == 0) {
        if (save_device(name, host) == 0) {
            message_page("DEVICE PAIRED",
                         "Device name and IP saved successfully.");
        } else {
            message_page("SAVE ERROR",
                         "Pairing succeeded, but saving failed.");
        }
    } else {
        message_page("PAIRING FAILED",
                     "Check the pairing code, port and IP.");
    }
}

/* ---------------------------------------------------------
   Connect / disconnect
   --------------------------------------------------------- */

static void connect_saved_device(int index)
{
    if (index < 0 || index >= device_count)
        return;

    Device d = devices[index];
    char serial[SERIAL_SIZE];

    if (find_active_serial(d.host, serial, sizeof(serial))) {
        if (!confirm_disconnect(d.name))
            return;

        char *args[] = {
            "adb", "disconnect", serial, NULL
        };

        int result = run_command(args);

        if (result == 0) {
            message_page("DISCONNECTED",
                         "ADB disconnect command completed.");
        } else {
            message_page("DISCONNECT FAILED",
                         "ADB could not disconnect this device.");
        }

        return;
    }

    char port[16];
    char address[INPUT_SIZE + 20];

    if (!read_field("Wireless debugging connection port:",
                    port, sizeof(port)))
        return;

    if (!valid_port(port)) {
        message_page("INVALID PORT",
                     "Enter a valid connection port.");
        return;
    }

    snprintf(address, sizeof(address), "%s:%s", d.host, port);

    char *args[] = {
        "adb", "connect", address, NULL
    };

    int result = run_command(args);

    if (result == 0) {
        message_page("CONNECTION COMMAND SENT",
                     "The device list will refresh.");
    } else {
        message_page("CONNECTION FAILED",
                     "Check the IP, port and wireless debugging.");
    }
}

static void saved_device_list(void)
{
    while (1) {
        load_phonebook();

        if (device_count == 0) {
            message_page("SAVED DEVICES",
                         "No saved devices. Pair a device first.");
            return;
        }

        char labels[MAX_DEVICES][INPUT_SIZE + 64];
        const char *items[MAX_DEVICES + 2];
        char serial[SERIAL_SIZE];

        for (int i = 0; i < device_count; i++) {
            if (find_active_serial(devices[i].host,
                                   serial, sizeof(serial))) {
                snprintf(labels[i], sizeof(labels[i]),
                         "%s: DISCONNECT", devices[i].name);
            } else {
                snprintf(labels[i], sizeof(labels[i]),
                         "%s: CONNECT", devices[i].name);
            }

            items[i] = labels[i];
        }

        items[device_count] = "Remove a saved device";
        items[device_count + 1] = "Back";

        int selected = menu("SAVED DEVICES",
                            items, device_count + 2);

        if (selected < 0 || selected == device_count + 1)
            return;

        if (selected == device_count) {
            const char *remove_items[MAX_DEVICES + 1];

            for (int i = 0; i < device_count; i++)
                remove_items[i] = devices[i].name;

            remove_items[device_count] = "Cancel";

            int target = menu("REMOVE SAVED DEVICE",
                              remove_items, device_count + 1);

            if (target >= 0 && target < device_count) {
                char prompt[INPUT_SIZE + 64];

                snprintf(prompt, sizeof(prompt),
                         "Remove %.150s from saved devices?",
                         devices[target].name);

                const char *confirm_items[] = {
                    "No",
                    "Yes, remove"
                };

                if (menu(prompt, confirm_items, 2) == 1) {
                    if (remove_device(target) == 0) {
                        message_page("DEVICE REMOVED",
                                     "Saved device entry removed.");
                    } else {
                        message_page("SAVE ERROR",
                                     "Could not update saved devices.");
                    }
                }
            }

            continue;
        }

        connect_saved_device(selected);
    }
}

/* ---------------------------------------------------------
   APK utilities
   --------------------------------------------------------- */

static int is_apk(const char *name)
{
    size_t n = strlen(name);

    return n >= 4 &&
           strcasecmp(name + n - 4, ".apk") == 0;
}

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int download_file(const char *url,
                         const char *destination)
{
    char temporary[PATH_SIZE];

    if (snprintf(temporary, sizeof(temporary),
                 "%s.part", destination)
        >= (int)sizeof(temporary))
        return -1;

    unlink(temporary);

    char *args[] = {
        "curl",
        "-fL",
        "--retry", "2",
        "--connect-timeout", "20",
        "-o", temporary,
        (char *)url,
        NULL
    };

    int result = run_command(args);

    if (result != 0) {
        unlink(temporary);
        return -1;
    }

    if (!file_exists(temporary)) {
        unlink(temporary);
        return -1;
    }

    if (rename(temporary, destination) != 0) {
        unlink(temporary);
        return -1;
    }

    return 0;
}

static int get_github_asset_url(const RecommendedApp *app,
                                char *url, size_t url_size)
{
    char *curl_args[] = {
        "curl",
        "-fsSL",
        "-H", "Accept: application/vnd.github+json",
        (char *)app->api_url,
        NULL
    };

    char *json = malloc(CAPTURE_SIZE);

    if (!json)
        return -1;

    int result = capture_command(curl_args, json, CAPTURE_SIZE);

    if (result != 0) {
        free(json);
        return -1;
    }

    char *jq_args[] = {
        "jq",
        "-r",
        (char *)app->jq_filter,
        NULL
    };

    char output[PATH_SIZE];

    result = capture_input(jq_args, json,
                           output, sizeof(output));

    free(json);

    if (result != 0)
        return -1;

    output[strcspn(output, "\r\n")] = '\0';

    if (!*output || strcmp(output, "null") == 0)
        return -1;

    if (strlen(output) >= url_size)
        return -1;

    snprintf(url, url_size, "%s", output);

    return 0;
}

static int get_app_download_url(const RecommendedApp *app,
                                char *url, size_t url_size)
{
    if (strcmp(app->name, "F-Droid") == 0) {
        snprintf(url, url_size,
                 "%s", "https://f-droid.org/F-Droid.apk");
        return 0;
    }

    return get_github_asset_url(app, url, url_size);
}

static int download_recommended_app(int index,
                                    char *apk_path,
                                    size_t apk_path_size)
{
    if (index < 0 || index >= RECOMMENDED_COUNT)
        return -1;

    const RecommendedApp *app = &recommended_apps[index];

    char url[PATH_SIZE];

    if (get_app_download_url(app, url, sizeof(url)) != 0) {
        message_page("DOWNLOAD LINK ERROR",
                     "Could not find the official APK release asset.");
        return -1;
    }

    if (snprintf(apk_path, apk_path_size,
                 "%s/%s", apk_dir, app->filename)
        >= (int)apk_path_size)
        return -1;

    char prompt[INPUT_SIZE + 64];

    snprintf(prompt, sizeof(prompt),
             "Download %s from its official source?", app->name);

    const char *items[] = {
        "Download APK",
        "Cancel"
    };

    if (menu("CONFIRM DOWNLOAD", items, 2) != 0)
        return -1;

    if (download_file(url, apk_path) != 0) {
        message_page("DOWNLOAD FAILED",
                     "Check your connection and try again.");
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------
   ADB target selection and APK installation
   --------------------------------------------------------- */

static int choose_adb_device(char serials[][SERIAL_SIZE],
                             int count)
{
    if (count <= 0)
        return -1;

    char labels[MAX_DEVICES][INPUT_SIZE + SERIAL_SIZE + 8];
    const char *items[MAX_DEVICES + 1];

    for (int i = 0; i < count; i++) {
        snprintf(labels[i], sizeof(labels[i]),
                 "%s | %s",
                 friendly_name(serials[i]),
                 serials[i]);

        items[i] = labels[i];
    }

    items[count] = "Cancel";

    int selected = menu("CHOOSE INSTALLATION TARGET",
                        items, count + 1);

    if (selected < 0 || selected >= count)
        return -1;

    return selected;
}

static int package_is_installed(const char *serial,
                                const char *package)
{
    char *args[] = {
        "adb", "-s", (char *)serial,
        "shell", "pm", "path", (char *)package,
        NULL
    };

    char output[4096];

    int result = capture_command(args, output, sizeof(output));

    return result == 0 && strncmp(output, "package:", 8) == 0;
}

static void install_apk_on_device(const char *apk_path,
                                  const char *package,
                                  const char *app_name)
{
    struct stat st;

    if (!check_adb())
        return;

    if (stat(apk_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        message_page("INVALID APK",
                     "The selected APK file is unavailable.");
        return;
    }

    char serials[MAX_DEVICES][SERIAL_SIZE];

    int count = get_active_devices(serials, MAX_DEVICES);

    if (count < 0) {
        message_page("ADB ERROR",
                     "Could not read the ADB device list.");
        return;
    }

    if (count == 0) {
        message_page("NO CONNECTED DEVICES",
                     "Connect an ADB device before installing.");
        return;
    }

    int selected = choose_adb_device(serials, count);

    if (selected < 0)
        return;

    int installed = 0;

    if (package && *package)
        installed = package_is_installed(serials[selected], package);

    char prompt[INPUT_SIZE + 80];

    snprintf(prompt, sizeof(prompt),
             "%s %s on this device?",
             installed ? "Update" : "Install", app_name);

    const char *items[] = {
        "Continue",
        "Cancel"
    };

    if (menu(prompt, items, 2) != 0)
        return;

    char *args[] = {
        "adb",
        "-s", serials[selected],
        "install", "-r",
        (char *)apk_path,
        NULL
    };

    int result = run_command(args);

    if (result == 0) {
        message_page("INSTALLATION FINISHED",
                     "ADB completed the installation command.");
    } else {
        message_page("INSTALLATION FAILED",
                     "Check ADB output; the existing app was not uninstalled.");
    }
}

static void recommended_apps_menu(void)
{
    while (1) {
        const char *items[] = {
            "Termux:X11",
            "InstallerX Revived",
            "F-Droid",
            "Back"
        };

        int selected = menu("RECOMMENDED APPS",
                            items, 4);

        if (selected < 0 || selected == 3)
            return;

        if (!check_download_tools())
            return;

        if (!check_adb())
            return;

        char apk_path[PATH_SIZE];

        if (download_recommended_app(selected,
                                     apk_path,
                                     sizeof(apk_path)) != 0)
            continue;

        const RecommendedApp *app = &recommended_apps[selected];

        install_apk_on_device(apk_path,
                              app->package,
                              app->name);
    }
}

/* ---------------------------------------------------------
   General APK installation
   --------------------------------------------------------- */

static void install_apk(const char *apk_path)
{
    install_apk_on_device(apk_path, NULL, "selected APK");
}

/* ---------------------------------------------------------
   APK file browser
   --------------------------------------------------------- */

static void browse_apks(void)
{
    char path[PATH_SIZE] = "/storage/emulated/0";

    while (1) {
        DIR *dir = opendir(path);

        if (!dir) {
            message_page("CANNOT OPEN DIRECTORY",
                         "Check Termux storage permission.");
            return;
        }

        char *names[MAX_ITEMS];
        int directories[MAX_ITEMS];
        int count = 0;
        int truncated = 0;

        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL) {
            if (!strcmp(entry->d_name, ".") ||
                !strcmp(entry->d_name, ".."))
                continue;

            char full[PATH_SIZE];

            int n = snprintf(full, sizeof(full),
                             "%s/%s", path, entry->d_name);

            if (n < 0 || (size_t)n >= sizeof(full))
                continue;

            struct stat st;

            if (stat(full, &st) != 0)
                continue;

            int is_directory = S_ISDIR(st.st_mode);

            if (!is_directory && !is_apk(entry->d_name))
                continue;

            if (count >= MAX_ITEMS) {
                truncated = 1;
                break;
            }

            names[count] = strdup(entry->d_name);

            if (!names[count])
                continue;

            directories[count] = is_directory;
            count++;
        }

        closedir(dir);

        char labels[MAX_ITEMS][INPUT_SIZE + 64];
        const char *items[MAX_ITEMS + 2];

        items[0] = "[ .. ]";

        for (int i = 0; i < count; i++) {
            snprintf(labels[i], sizeof(labels[i]),
                     "%s%s%s",
                     directories[i] ? "[DIR] " : "",
                     names[i],
                     directories[i] ? "/" : "");

            items[i + 1] = labels[i];
        }

        items[count + 1] = "Back";

        char title[INPUT_SIZE + 64];

        snprintf(title, sizeof(title),
                 "BROWSE: %.180s%s",
                 path, truncated ? " [LIST LIMITED]" : "");

        int choice = menu(title, items, count + 2);

        if (choice == count + 1) {
            for (int i = 0; i < count; i++)
                free(names[i]);

            return;
        }

        if (choice == 0) {
            if (strcmp(path, "/") != 0) {
                char *slash = strrchr(path, '/');

                if (slash == path)
                    path[1] = '\0';
                else if (slash)
                    *slash = '\0';
            }
        } else if (choice > 0 && choice <= count) {
            int index = choice - 1;
            char selected[PATH_SIZE];

            int n = snprintf(selected, sizeof(selected),
                             "%s/%s", path, names[index]);

            if (n >= 0 && (size_t)n < sizeof(selected)) {
                if (directories[index]) {
                    snprintf(path, sizeof(path), "%s", selected);
                } else {
                    for (int i = 0; i < count; i++)
                        free(names[i]);

                    install_apk(selected);
                    continue;
                }
            }
        }

        for (int i = 0; i < count; i++)
            free(names[i]);
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

    if (snprintf(config_dir, sizeof(config_dir),
                 "%s/.config/cli-ins", home)
        >= (int)sizeof(config_dir)) {
        fprintf(stderr, "Configuration path too long.\n");
        return 1;
    }

    if (snprintf(phonebook_path, sizeof(phonebook_path),
                 "%s/devices.tsv", config_dir)
        >= (int)sizeof(phonebook_path)) {
        fprintf(stderr, "Phonebook path too long.\n");
        return 1;
    }

    if (snprintf(apk_dir, sizeof(apk_dir),
                 "%s/.local/share/cli-ins/apks", home)
        >= (int)sizeof(apk_dir)) {
        fprintf(stderr, "APK directory path too long.\n");
        return 1;
    }

    if (ensure_directory_tree() != 0) {
        fprintf(stderr,
                "Could not create CLI-INS configuration directories.\n");
        return 1;
    }

    load_phonebook();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors())
        init_theme();

    mousemask(ALL_MOUSE_EVENTS, NULL);
    mouseinterval(150);

    const char *items[] = {
        "Pair a device",
        "Connect to a device",
        "Browse folders / install APK",
        "Recommended Apps",
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
            browse_apks();
            break;

        case 3:
            recommended_apps_menu();
            break;

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
