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
#include <sys/wait.h>
#include <errno.h>

#define APP_NAME "CLI-INS"
#define APP_VERSION "0.4"
#define MAX_ITEMS 512
#define PATH_SIZE 4096
#define INPUT_SIZE 256
#define MAX_DEVICES 256
#define SERIAL_SIZE 512

enum {
    CP_NORMAL = 1, CP_TITLE, CP_SELECTED, CP_MUTED,
    CP_ACCENT, CP_BORDER, CP_INPUT
};

typedef struct {
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
} Device;

static Device devices[MAX_DEVICES];
static int device_count;
static char config_dir[PATH_SIZE];
static char phonebook_path[PATH_SIZE];
static int use_colors;

/* ---------------------------------------------------------
   Configuration
   --------------------------------------------------------- */

static void ensure_config_dir(void)
{
    char path[PATH_SIZE];
    const char *home = getenv("HOME");

    if (!home)
        return;

    snprintf(path, sizeof(path), "%s/.config", home);
    mkdir(path, 0700);
    mkdir(config_dir, 0700);
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

static int save_phonebook(void)
{
    FILE *fp = fopen(phonebook_path, "w");
    if (!fp)
        return -1;

    for (int i = 0; i < device_count; i++)
        fprintf(fp, "%s\t%s\n",
                devices[i].name, devices[i].host);

    return fclose(fp) == 0 ? 0 : -1;
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
                 sizeof(devices[device_count].name), "%s", name);

        snprintf(devices[device_count].host,
                 sizeof(devices[device_count].host), "%s", host);

        device_count++;
    }

    fclose(fp);
    (void)save_phonebook();
}

static int save_device(const char *name, const char *host)
{
    if (!safe_name(name) || !valid_host(host))
        return -1;

    for (int i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].host, host)) {
            snprintf(devices[i].name,
                     sizeof(devices[i].name), "%s", name);
            return save_phonebook();
        }
    }

    if (device_count >= MAX_DEVICES)
        return -1;

    snprintf(devices[device_count].name,
             sizeof(devices[device_count].name), "%s", name);

    snprintf(devices[device_count].host,
             sizeof(devices[device_count].host), "%s", host);

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
    centered(2, "ANDROID APK INSTALLATION MANAGER", CP_MUTED);

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

    if (cols >= 60)
        centered(rows - 3,
                 "Made by precisescope (GitHub) | 8bit_binary (Instagram)",
                 CP_MUTED);
    else {
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

        draw_box(panel_y, panel_x, panel_height, panel_width);

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
                mvhline(y, panel_x + 1, ' ', panel_width - 2);
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
   External commands
   --------------------------------------------------------- */

static int run_command(char *const argv[])
{
    pid_t pid;
    int status = -1;

    fflush(NULL);
    endwin();

    printf("\n$");
    for (int i = 0; argv[i]; i++)
        printf(" %s", argv[i]);
    printf("\n\n");
    fflush(stdout);

    pid = fork();

    if (pid == 0) {
        execvp(argv[0], argv);
        perror("Could not start command");
        _exit(127);
    }

    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                status = -1;
                break;
            }
        }
    } else {
        perror("fork");
    }

    if (pid > 0 && status != -1 && WIFEXITED(status))
        printf("\nADB exit code: %d\n", WEXITSTATUS(status));
    else
        printf("\nADB command did not finish normally.\n");

    printf("\nPress Enter to return...");
    fflush(stdout);

    int ch;
    do {
        ch = getchar();
    } while (ch != '\n' && ch != EOF);

    reset_prog_mode();
    clear();
    refresh();

    if (pid < 0 || status == -1 || !WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
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

        snprintf(candidate, sizeof(candidate),
                 "%s/%s", token, command);

        if (access(candidate, X_OK) == 0) {
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

/* ---------------------------------------------------------
   ADB device discovery
   --------------------------------------------------------- */

static int find_active_serial(const char *host,
                              char *serial,
                              size_t serial_size)
{
    FILE *fp = popen("adb devices", "r");
    if (!fp)
        return 0;

    char line[1024];
    int found = 0;
    size_t host_len = strlen(host);

    while (fgets(line, sizeof(line), fp)) {
        char listed[SERIAL_SIZE];
        char state[128];

        if (sscanf(line, "%511s %127s", listed, state) != 2)
            continue;

        if (strcmp(state, "device") != 0)
            continue;

        if (strncmp(listed, host, host_len) != 0 ||
            listed[host_len] != ':')
            continue;

        if (snprintf(serial, serial_size, "%s", listed) >=
            (int)serial_size)
            continue;

        found = 1;
        break;
    }

    pclose(fp);
    return found;
}

/* Enumerate all active ADB devices, including devices that
   have not been saved in the CLI-INS phonebook. */

static int get_active_devices(char serials[][SERIAL_SIZE],
                              int max_devices)
{
    FILE *fp = popen("adb devices", "r");
    if (!fp)
        return 0;

    char line[1024];
    int count = 0;

    while (fgets(line, sizeof(line), fp) &&
           count < max_devices) {
        char listed[SERIAL_SIZE];
        char state[128];

        if (sscanf(line, "%511s %127s", listed, state) != 2)
            continue;

        if (strcmp(state, "device") != 0)
            continue;

        snprintf(serials[count], SERIAL_SIZE, "%s", listed);
        count++;
    }

    pclose(fp);
    return count;
}

static const char *friendly_name(const char *serial)
{
    static char result[INPUT_SIZE];

    load_phonebook();

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
   Pairing
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
        !read_field("Six-digit pairing code:",
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

    if (result == 0) {
        if (save_device(name, host) == 0)
            message_page("DEVICE PAIRED",
                         "Device name and IP saved successfully.");
        else
            message_page("SAVE ERROR",
                         "Pairing succeeded, but saving failed.");
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

        if (result == 0)
            message_page("DISCONNECTED",
                         "ADB disconnect command completed.");
        else
            message_page("DISCONNECT FAILED",
                         "ADB could not disconnect this device.");

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

    if (result == 0)
        message_page("CONNECTION COMMAND SENT",
                     "The device list will refresh.");
    else
        message_page("CONNECTION FAILED",
                     "Check the IP, port and wireless debugging.");
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
                    if (remove_device(target) == 0)
                        message_page("DEVICE REMOVED",
                                     "Saved device entry removed.");
                    else
                        message_page("SAVE ERROR",
                                     "Could not update saved devices.");
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

static int is_apk(const char *name)
{
    size_t n = strlen(name);

    return n >= 4 &&
           strcasecmp(name + n - 4, ".apk") == 0;
}

static void install_apk(const char *apk_path)
{
    struct stat st;

    if (!check_adb())
        return;

    if (stat(apk_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        message_page("INVALID APK",
                     "The selected file is unavailable.");
        return;
    }

    char serials[MAX_DEVICES][SERIAL_SIZE];
    char labels[MAX_DEVICES][INPUT_SIZE + SERIAL_SIZE + 8];
    const char *items[MAX_DEVICES + 1];

    int count = get_active_devices(serials, MAX_DEVICES);

    if (count == 0) {
        message_page("NO CONNECTED DEVICES",
                     "Connect an ADB device before installing.");
        return;
    }

    for (int i = 0; i < count; i++) {
        snprintf(labels[i], sizeof(labels[i]),
                 "%s | %s",
                 friendly_name(serials[i]), serials[i]);

        items[i] = labels[i];
    }

    items[count] = "Cancel";

    int selected = menu("CHOOSE INSTALLATION TARGET",
                        items, count + 1);

    if (selected < 0 || selected >= count)
        return;

    const char *confirm_items[] = {
        "Install APK",
        "Cancel"
    };

    if (menu("CONFIRM APK INSTALLATION",
             confirm_items, 2) != 0)
        return;

    char *args[] = {
        "adb", "-s", serials[selected],
        "install", "-r", (char *)apk_path, NULL
    };

    int result = run_command(args);

    if (result == 0)
        message_page("INSTALLATION FINISHED",
                     "ADB completed the installation command.");
    else
        message_page("INSTALLATION FAILED",
                     "Check the ADB output for the error.");
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

        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL &&
               count < MAX_ITEMS) {
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

        /* IMPORTANT: Back is now a separate menu entry. */
        items[count + 1] = "Back";

        char title[INPUT_SIZE + 64];
        snprintf(title, sizeof(title),
                 "BROWSE: %.180s", path);

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
                 "%s/.config/cli-ins", home) >=
        (int)sizeof(config_dir)) {
        fprintf(stderr, "Configuration path too long.\n");
        return 1;
    }

    if (snprintf(phonebook_path, sizeof(phonebook_path),
                 "%s/devices.tsv", config_dir) >=
        (int)sizeof(phonebook_path)) {
        fprintf(stderr, "Phonebook path too long.\n");
        return 1;
    }

    ensure_config_dir();
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
        "Exit"
    };

    int running = 1;

    while (running) {
        int choice = menu("MAIN MENU", items, 4);

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
