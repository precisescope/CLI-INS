
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

#define MAX_ITEMS 512
#define PATH_SIZE 4096
#define INPUT_SIZE 256
#define MAX_DEVICES 256

typedef struct {
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
    char port[16];
} Device;

static Device devices[MAX_DEVICES];
static int device_count = 0;
static char config_dir[PATH_SIZE];
static char phonebook_path[PATH_SIZE];

static void pause_message(const char *message)
{
    mvprintw(LINES - 3, 2, "%s", message);
    refresh();
    (void)getch();
}

static void header(const char *subtitle)
{
    erase();

    mvprintw(1, 2, "CLI installer for Android");
    mvprintw(2, 2, "ver 0.1 | November 2026");
    mvprintw(3, 2, "%s", subtitle);

    if (LINES > 5 && COLS > 4)
        mvhline(4, 2, ACS_HLINE, COLS - 4);
}

static void wait_for_enter(void)
{
    int ch;

    echo();
    mvprintw(LINES - 2, 2, "Press Enter to return...");
    refresh();

    do {
        ch = getch();
    } while (ch != '\n' && ch != KEY_ENTER && ch != ERR);

    noecho();
}

static int menu(const char *title,
                const char *items[], int count)
{
    int selected = 0;

    if (count <= 0)
        return 0;

    while (1) {
        header(title);
        mvprintw(6, 2, "UP/DOWN: Navigate  ENTER: Select");

        int visible = LINES - 12;
        if (visible < 1)
            visible = 1;

        int start = 0;
        if (selected >= visible)
            start = selected - visible + 1;

        for (int i = start; i < count && i < start + visible; i++) {
            int row = 8 + i - start;

            if (i == selected)
                attron(A_REVERSE);

            mvprintw(row, 4, "%.*s",
                     COLS > 8 ? COLS - 8 : 1, items[i]);

            if (i == selected)
                attroff(A_REVERSE);
        }

        if (LINES >= 2)
            mvprintw(LINES - 2, 2,
                     "precisescope (GitHub) | 8bit_binary");

        refresh();

        int key = getch();

        if (key == KEY_UP)
            selected = (selected + count - 1) % count;
        else if (key == KEY_DOWN)
            selected = (selected + 1) % count;
        else if (key == '\n' || key == KEY_ENTER || key == KEY_RIGHT)
            return selected;
        else if (key == 27)
            return count - 1;
    }
}

static void read_field(const char *label,
                       char *buffer, size_t size)
{
    buffer[0] = '\0';

    echo();
    curs_set(1);

    mvprintw(LINES - 5, 2, "%.*s",
             COLS > 4 ? COLS - 4 : 1, label);
    clrtoeol();

    move(LINES - 4, 2);
    clrtoeol();
    getnstr(buffer, (int)size - 1);

    noecho();
    curs_set(0);
}

static int valid_host(const char *host)
{
    if (!*host)
        return 0;

    for (const char *p = host; *p; p++) {
        if (!(isalnum((unsigned char)*p) ||
              *p == '.' || *p == ':' ||
              *p == '-' || *p == '_'))
            return 0;
    }

    return 1;
}

static int valid_port(const char *port)
{
    if (!*port)
        return 0;

    for (const char *p = port; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }

    char *end = NULL;
    long value = strtol(port, &end, 10);

    return end && *end == '\0' && value >= 1 && value <= 65535;
}

static int safe_name(const char *name)
{
    if (!*name)
        return 0;

    for (const char *p = name; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r' ||
            *p == '|')
            return 0;
    }

    return 1;
}

/* Execute a program directly, without passing arguments through a shell. */
static int run_command(char *const argv[])
{
    endwin();

    printf("\n$");
    for (int i = 0; argv[i]; i++)
        printf(" %s", argv[i]);
    printf("\n\n");

    pid_t pid = fork();

    if (pid == 0) {
        execvp(argv[0], argv);
        perror("Could not start command");
        _exit(127);
    }

    int status = -1;

    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    } else {
        perror("fork");
    }

    printf("\nPress Enter to return...");
    int ch;
    do {
        ch = getchar();
    } while (ch != '\n' && ch != EOF);

    reset_prog_mode();
    refresh();
    clear();

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

    fclose(fp);
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
            !safe_name(name) || !valid_host(host) ||
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
                       const char *host, const char *port)
{
    if (!safe_name(name) || !valid_host(host) ||
        !valid_port(port))
        return -1;

    for (int i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].host, host)) {
            snprintf(devices[i].name, sizeof(devices[i].name),
                     "%s", name);
            snprintf(devices[i].port, sizeof(devices[i].port),
                     "%s", port);
            return save_phonebook();
        }
    }

    if (device_count >= MAX_DEVICES)
        return -1;

    Device *d = &devices[device_count++];

    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->host, sizeof(d->host), "%s", host);
    snprintf(d->port, sizeof(d->port), "%s", port);

    return save_phonebook();
}

static void pair_device(void)
{
    char host[INPUT_SIZE];
    char port[16];
    char code[INPUT_SIZE];

    header("Pair a device");
    read_field("Device IP address:", host, sizeof(host));
    read_field("Pairing port:", port, sizeof(port));
    read_field("Pairing code:", code, sizeof(code));

    if (!valid_host(host) || !valid_port(port) ||
        !*code || strpbrk(code, " \t\r\n")) {
        header("Invalid input");
        pause_message("Check the address, port and pairing code.");
        return;
    }

    char address[INPUT_SIZE + 20];
    snprintf(address, sizeof(address), "%s:%s", host, port);

    char *args[] = { "adb", "pair", address, code, NULL };
    (void)run_command(args);
}

static void connect_device(void)
{
    char name[INPUT_SIZE];
    char host[INPUT_SIZE];
    char port[16];

    header("Connect to a device");
    read_field("Friendly device name:", name, sizeof(name));
    read_field("Device IP address:", host, sizeof(host));
    read_field("Connection port:", port, sizeof(port));

    if (!safe_name(name) || !valid_host(host) ||
        !valid_port(port)) {
        header("Invalid input");
        pause_message("Check the name, address and port.");
        return;
    }

    char address[INPUT_SIZE + 20];
    snprintf(address, sizeof(address), "%s:%s", host, port);

    char *args[] = { "adb", "connect", address, NULL };
    int result = run_command(args);

    if (result == 0) {
        if (save_device(name, host, port) == 0) {
            header("Phonebook updated");
            pause_message("Device saved to the phonebook.");
        } else {
            header("Phonebook error");
            pause_message("Connected, but could not save the device.");
        }
    }
}

static int is_apk(const char *name)
{
    size_t n = strlen(name);

    return n >= 4 &&
           strcasecmp(name + n - 4, ".apk") == 0;
}

static void install_apk(const char *apk_path);

static void browse_apks(void)
{
    char path[PATH_SIZE] = "/storage/emulated/0";

    while (1) {
        DIR *dir = opendir(path);

        if (!dir) {
            header("Cannot open directory");
            mvprintw(7, 2, "%.*s",
                     COLS > 4 ? COLS - 4 : 1, path);
            pause_message("Check Termux storage permission.");
            return;
        }

        char *names[MAX_ITEMS];
        int is_directory[MAX_ITEMS];
        int count = 0;

        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL &&
               count < MAX_ITEMS) {
            if (!strcmp(entry->d_name, ".") ||
                !strcmp(entry->d_name, ".."))
                continue;

            char full[PATH_SIZE];

            int n = snprintf(full, sizeof(full), "%s/%s",
                             path, entry->d_name);

            if (n < 0 || (size_t)n >= sizeof(full))
                continue;

            struct stat st;

            if (stat(full, &st) != 0)
                continue;

            int directory = S_ISDIR(st.st_mode);

            if (!directory && !is_apk(entry->d_name))
                continue;

            char *copy = strdup(entry->d_name);

            if (!copy)
                continue;

            names[count] = copy;
            is_directory[count] = directory;
            count++;
        }

        closedir(dir);

        const char *actions[MAX_ITEMS + 1];
        char labels[MAX_ITEMS][PATH_SIZE];

        actions[0] = "[ .. ]";

        for (int i = 0; i < count; i++) {
            snprintf(labels[i], sizeof(labels[i]),
                     "%s%s%s",
                     is_directory[i] ? "[DIR] " : "",
                     names[i],
                     is_directory[i] ? "/" : "");

            actions[i + 1] = labels[i];
        }

        char title[PATH_SIZE + 32];
        snprintf(title, sizeof(title), "Browse: %.4000s", path);

        int choice = menu(title, actions, count + 1);

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
                if (is_directory[index]) {
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

static int adb_serial(const Device *d, char *out, size_t size)
{
    int n = snprintf(out, size, "%s:%s", d->host, d->port);
    return n >= 0 && (size_t)n < size;
}

static void install_apk(const char *apk_path)
{
    const char *choices[] = {
        "Choose connected target",
        "Cancel"
    };

    struct stat st;

    if (stat(apk_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        header("Invalid APK");
        pause_message("The selected file is no longer available.");
        return;
    }

    int choice = menu("Install APK?", choices, 2);

    if (choice != 0)
        return;

    char *output[MAX_ITEMS];
    char labels[MAX_ITEMS][INPUT_SIZE + 128];
    char *serials[MAX_ITEMS];
    int count = 0;

    /* Use the phonebook as a convenient list of known targets. */
    for (int i = 0; i < device_count && count < MAX_ITEMS; i++) {
        char serial[INPUT_SIZE + 20];

        if (!adb_serial(&devices[i], serial, sizeof(serial)))
            continue;

        serials[count] = strdup(serial);

        if (!serials[count])
            continue;

        snprintf(labels[count], sizeof(labels[count]),
                 "%s (%s:%s)",
                 devices[i].name, devices[i].host, devices[i].port);

        output[count] = labels[count];
        count++;
    }

    if (count == 0) {
        header("No saved devices");
        pause_message("Connect a device first, then try again.");
        return;
    }

    int selected = menu("Choose installation target",
                        (const char **)output, count);

    if (selected < 0 || selected >= count) {
        for (int i = 0; i < count; i++)
            free(serials[i]);
        return;
    }

    char target[INPUT_SIZE + 20];
    snprintf(target, sizeof(target), "%s", serials[selected]);

    for (int i = 0; i < count; i++)
        free(serials[i]);

    char *args[] = {
        "adb", "-s", target, "install", "-r",
        (char *)apk_path, NULL
    };

    (void)run_command(args);
}

static void phonebook(void)
{
    while (1) {
        load_phonebook();

        if (device_count == 0) {
            header("Device phonebook");
            mvprintw(7, 2, "No saved devices yet.");
            pause_message("Use Connect to a device to add one.");
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

        int selected = menu("Device phonebook",
                            items, device_count + 1);

        if (selected == device_count)
            return;

        Device *d = &devices[selected];

        const char *actions[] = {
            "Connect / reconnect",
            "Remove from phonebook",
            "Back"
        };

        char title[INPUT_SIZE + 64];
        snprintf(title, sizeof(title), "%.200s", d->name);

        int action = menu(title, actions, 3);

        if (action == 0) {
            char address[INPUT_SIZE + 20];
            snprintf(address, sizeof(address), "%s:%s",
                     d->host, d->port);

            char *args[] = { "adb", "connect", address, NULL };
            (void)run_command(args);
        } else if (action == 1) {
            for (int i = selected; i < device_count - 1; i++)
                devices[i] = devices[i + 1];

            device_count--;
            (void)save_phonebook();
        }
    }
}

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
        fprintf(stderr, "Configuration path is too long.\n");
        return 1;
    }

    if (snprintf(phonebook_path, sizeof(phonebook_path),
                 "%s/devices.tsv", config_dir) >=
        (int)sizeof(phonebook_path)) {
        fprintf(stderr, "Phonebook path is too long.\n");
        return 1;
    }

    (void)mkdir(home, 0700);

    char parent[PATH_SIZE];
    snprintf(parent, sizeof(parent), "%s/.config", home);
    (void)mkdir(parent, 0700);
    (void)mkdir(config_dir, 0700);

    load_phonebook();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    const char *items[] = {
        "Pair a device",
        "Connect to a device",
        "Browse folders / install APK",
        "Device phonebook",
        "Exit"
    };

    int running = 1;

    while (running) {
        int choice = menu("Main menu", items, 5);

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
    return 0;
}

