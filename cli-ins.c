
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
#include <limits.h>
#include <errno.h>
#include <signal.h>

#define APP_NAME "CLI-INS"
#define APP_VERSION "0.2"

#define MAX_ITEMS 512
#define PATH_SIZE 4096
#define INPUT_SIZE 256
#define MAX_DEVICES 256

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
    char port[16];
} Device;

static Device devices[MAX_DEVICES];
static int device_count = 0;

static char config_dir[PATH_SIZE];
static char phonebook_path[PATH_SIZE];

static int use_colors = 0;

/* ---------------------------------------------------------
   Configuration and phonebook
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
    char *end = NULL;
    long value;

    if (!port || !*port)
        return 0;

    for (const char *p = port; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }

    errno = 0;
    value = strtol(port, &end, 10);

    return errno == 0 && end && *end == '\0' &&
           value >= 1 && value <= 65535;
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

    for (int i = 0; i < device_count; i++) {
        fprintf(fp, "%s\t%s\t%s\n",
                devices[i].name,
                devices[i].host,
                devices[i].port);
    }

    if (fclose(fp) != 0)
        return -1;

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
        char *port = strtok(NULL, "\t");

        if (!name || !host || !port ||
            !safe_name(name) ||
            !valid_host(host) ||
            !valid_port(port))
            continue;

        Device *d = &devices[device_count];

        snprintf(d->name, sizeof(d->name), "%s", name);
        snprintf(d->host, sizeof(d->host), "%s", host);
        snprintf(d->port, sizeof(d->port), "%s", port);

        device_count++;
    }

    fclose(fp);
}

static int save_device(const char *name,
                       const char *host,
                       const char *port)
{
    if (!safe_name(name) ||
        !valid_host(host) ||
        !valid_port(port))
        return -1;

    for (int i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].host, host)) {
            snprintf(devices[i].name,
                     sizeof(devices[i].name), "%s", name);

            snprintf(devices[i].port,
                     sizeof(devices[i].port), "%s", port);

            return save_phonebook();
        }
    }

    if (device_count >= MAX_DEVICES)
        return -1;

    Device *d = &devices[device_count++];

    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->host, sizeof(d->host), "%s", host);
    snprintf(d->port, sizeof(d->port), "%s", port);

    if (save_phonebook() != 0) {
        device_count--;
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

static void draw_box(int y, int x, int height, int width)
{
    if (height < 2 || width < 2)
        return;

    color_on(CP_BORDER);

    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);

    for (int i = 1; i < width - 1; i++) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + height - 1, x + i, ACS_HLINE);
    }

    for (int i = 1; i < height - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + width - 1, ACS_VLINE);
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

    const char *navigation =
        "ARROWS: Navigate   ENTER: Select   ESC: Back";

    const char *credits =
        "Made by precisescope (GitHub) | 8bit_binary (Instagram)";

    if (cols >= (int)strlen(credits) + 2) {
        centered(rows - 4, navigation, CP_MUTED);
        centered(rows - 3, credits, CP_MUTED);
    } else {
        centered(rows - 5, navigation, CP_MUTED);
        centered(rows - 4, "GitHub: precisescope", CP_MUTED);
        centered(rows - 3, "Instagram: 8bit_binary", CP_MUTED);
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

static void message_page(const char *title,
                         const char *message)
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

static void read_field(const char *label,
                       char *buffer,
                       size_t size)
{
    int rows, cols;

    getmaxyx(stdscr, rows, cols);

    header("INPUT");

    color_on(CP_TITLE);

    if (rows > 8)
        mvaddnstr(8, 4, label, cols - 8);

    color_off(CP_TITLE);

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

    getnstr(buffer, (int)size - 1);

    noecho();
    curs_set(0);
}

/* ---------------------------------------------------------
   Menu: keyboard and terminal mouse
   --------------------------------------------------------- */

static int menu(const char *title,
                const char *items[],
                int count)
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

        /*
         * Reserve space for:
         * - Menu heading
         * - Menu options
         * - Top and bottom borders
         * - Footer
         */

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

        /* Menu heading */

        color_on(CP_MUTED);

        mvaddnstr(panel_y + 1,
                  panel_x + 3,
                  "SELECT AN OPTION",
                  panel_width - 6);

        color_off(CP_MUTED);

        /* Menu entries */

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
                          items[index],
                          panel_width - 8);

                color_off(CP_SELECTED);
            } else {
                color_on(CP_NORMAL);

                mvaddnstr(y, panel_x + 3, "  ", 2);

                mvaddnstr(y, panel_x + 5,
                          items[index],
                          panel_width - 8);

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

        case KEY_MOUSE:
            {
                MEVENT event;

                if (getmouse(&event) == OK &&
                    (event.bstate & BUTTON1_CLICKED)) {

                    int first_row = panel_y + 2;
                    int last_row = first_row + visible;

                    if (event.x >= panel_x &&
                        event.x < panel_x + panel_width &&
                        event.y >= first_row &&
                        event.y < last_row) {

                        int index = offset +
                                    event.y - first_row;

                        if (index >= 0 && index < count)
                            return index;
                    }
                }
            }
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
   ADB pairing and connection
   --------------------------------------------------------- */

static void pair_device(void)
{
    char host[INPUT_SIZE];
    char port[16];
    char code[INPUT_SIZE];
    char address[INPUT_SIZE + 20];

    if (!check_adb())
        return;

    read_field("Device IP address:", host, sizeof(host));
    read_field("Pairing port:", port, sizeof(port));
    read_field("Pairing code:", code, sizeof(code));

    if (!valid_host(host) ||
        !valid_port(port) ||
        !*code ||
        strpbrk(code, " \t\r\n")) {

        message_page("INVALID INPUT",
                     "Check the IP, port and pairing code.");
        return;
    }

    snprintf(address, sizeof(address),
             "%s:%s", host, port);

    char *args[] = {
        "adb", "pair", address, code, NULL
    };

    (void)run_command(args);
}

static void connect_device(void)
{
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
    char port[16];
    char address[INPUT_SIZE + 20];

    if (!check_adb())
        return;

    read_field("Friendly device name:", name, sizeof(name));
    read_field("Device IP address:", host, sizeof(host));
    read_field("Connection port:", port, sizeof(port));

    if (!safe_name(name) ||
        !valid_host(host) ||
        !valid_port(port)) {

        message_page("INVALID INPUT",
                     "Check the name, address and port.");
        return;
    }

    snprintf(address, sizeof(address),
             "%s:%s", host, port);

    char *args[] = {
        "adb", "connect", address, NULL
    };

    int result = run_command(args);

    if (result == 0) {
        if (save_device(name, host, port) == 0)
            message_page("PHONEBOOK UPDATED",
                         "Device saved successfully.");
        else
            message_page("PHONEBOOK ERROR",
                         "Could not save the device.");
    }
}

/* ---------------------------------------------------------
   Connected-device detection
   --------------------------------------------------------- */

static int device_is_connected(const char *serial)
{
    FILE *fp = popen("adb devices", "r");

    if (!fp)
        return 0;

    char line[1024];
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        char listed[INPUT_SIZE + 64];
        char state[128];

        if (sscanf(line, "%319s %127s",
                   listed, state) != 2)
            continue;

        if (!strcmp(listed, serial) &&
            !strcmp(state, "device")) {
            found = 1;
            break;
        }
    }

    pclose(fp);
    return found;
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

static int device_serial(const Device *d,
                         char *out, size_t size)
{
    int n = snprintf(out, size, "%s:%s",
                     d->host, d->port);

    return n >= 0 && (size_t)n < size;
}

static void install_apk(const char *apk_path)
{
    struct stat st;

    if (!check_adb())
        return;

    if (stat(apk_path, &st) != 0 ||
        !S_ISREG(st.st_mode)) {

        message_page("INVALID APK",
                     "The selected file is unavailable.");
        return;
    }

    load_phonebook();

    char serials[MAX_DEVICES][INPUT_SIZE + 20];
    char labels[MAX_DEVICES][INPUT_SIZE + 128];
    const char *items[MAX_DEVICES + 1];

    int count = 0;

    for (int i = 0; i < device_count; i++) {
        if (!device_serial(&devices[i],
                           serials[count],
                           sizeof(serials[count])))
            continue;

        if (!device_is_connected(serials[count]))
            continue;

        snprintf(labels[count], sizeof(labels[count]),
                 "%s | %s",
                 devices[i].name, serials[count]);

        items[count] = labels[count];
        count++;
    }

    if (count == 0) {
        message_page("NO CONNECTED DEVICES",
                     "Connect a saved device before installing.");
        return;
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

    int confirm = menu("CONFIRM APK INSTALLATION",
                       confirm_items, 2);

    if (confirm != 0)
        return;

    char *args[] = {
        "adb",
        "-s",
        serials[selected],
        "install",
        "-r",
        (char *)apk_path,
        NULL
    };

    (void)run_command(args);
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
        const char *items[MAX_ITEMS + 1];

        items[0] = "[ .. ]";

        for (int i = 0; i < count; i++) {
            snprintf(labels[i], sizeof(labels[i]),
                     "%s%s%s",
                     directories[i] ? "[DIR] " : "",
                     names[i],
                     directories[i] ? "/" : "");

            items[i + 1] = labels[i];
        }

        char title[INPUT_SIZE + 64];

        snprintf(title, sizeof(title),
                 "BROWSE: %.180s", path);

        int choice = menu(title, items, count + 1);

        if (choice == count) {
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
        } else {
            int index = choice - 1;
            char selected[PATH_SIZE];

            int n = snprintf(selected, sizeof(selected),
                             "%s/%s", path, names[index]);

            if (n >= 0 && (size_t)n < sizeof(selected)) {
                if (directories[index]) {
                    snprintf(path, sizeof(path),
                             "%s", selected);
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
   Device phonebook
   --------------------------------------------------------- */

static void phonebook(void)
{
    while (1) {
        load_phonebook();

        if (device_count == 0) {
            message_page("DEVICE PHONEBOOK",
                         "No saved devices. Connect one first.");
            return;
        }

        char labels[MAX_DEVICES][INPUT_SIZE + 128];
        const char *items[MAX_DEVICES + 1];

        for (int i = 0; i < device_count; i++) {
            snprintf(labels[i], sizeof(labels[i]),
                     "%s | %s:%s",
                     devices[i].name,
                     devices[i].host,
                     devices[i].port);

            items[i] = labels[i];
        }

        items[device_count] = "Back";

        int selected = menu("DEVICE PHONEBOOK",
                            items, device_count + 1);

        if (selected < 0 || selected >= device_count)
            return;

        const char *actions[] = {
            "Connect / reconnect",
            "Remove from phonebook",
            "Back"
        };

        int action = menu(devices[selected].name,
                          actions, 3);

        if (action == 0) {
            Device d = devices[selected];

            char address[INPUT_SIZE + 20];

            snprintf(address, sizeof(address),
                     "%s:%s", d.host, d.port);

            char *args[] = {
                "adb", "connect", address, NULL
            };

            (void)run_command(args);

        } else if (action == 1) {
            for (int i = selected;
                 i < device_count - 1; i++) {
                devices[i] = devices[i + 1];
            }

            device_count--;

            if (save_phonebook() == 0)
                message_page("DEVICE REMOVED",
                             "Phonebook updated.");
            else
                message_page("SAVE ERROR",
                             "Could not update phonebook.");
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
        "Device phonebook",
        "Exit"
    };

    int running = 1;

    while (running) {
        int choice = menu("MAIN MENU",
                          items,
                          sizeof(items) / sizeof(items[0]));

        switch (choice) {
        case 0:
            pair_device();
            break;

        case 1:
            connect_device();
            break;

        case 2:
            browse_apks();
            break;

        case 3:
            phonebook();
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

