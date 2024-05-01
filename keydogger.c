#include "keydogger.h"
#include <stdio.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <linux/uinput.h>

// Errors
#define EOPEN 1  // Cannot open
#define EREAD 2  // Cannot read
#define EINVCH 3 // Invalid character
#define EINVC 4  // Invalid keycode
#define EINIT 5  // Error initializing
#define EADD 6   // Error adding
#define ESETUP 7
#define ECREATE 8
#define EWRITE 9
#define EENV 10
#define EPERM 11

#define RC_PATH "./keydoggerrc"
#define UINPUT_PATH "/dev/uinput"

extern char **environ;

static char *KEYBOARD_DEVICE = KEYBOARD_EVENT_PATH;
static struct trie *TRIE = NULL;

void read_from_rc()
{
    FILE *rc_file = fopen(RC_PATH, "r");
    if (rc_file < 0)
    {
        printf("Error opening %s\n", RC_PATH);
        exit(EOPEN);
    }
    char *key[10], *value[100];
    while (fscanf(rc_file, "%99[^=]=%99[^\n]\n", key, value) == 2)
    {
        printf("%s %s\n", key, value);
        push_trie(key, value);
    }
    fclose(rc_file);
}

bool check_priveleges()
{
    if (environ == NULL)
    {
        printf("Error accessing ENV variables\n");
        exit(EENV);
    }
    int i = 0;
    while (environ[i] != NULL)
    {
        if (strncmp("USER=ROOT", environ[i], 9) == 0)
        {
            return true;
        }
        if (strncmp("SUDO_COMMAND", environ[i], 12) == 0)
        {
            return true;
        }
        i++;
    }
    return false;
}

bool valid_key_code(size_t code)
{
    for (size_t i = 0; i < READABLE_KEYS; i++)
    {
        if (key_codes[i] == code)
            return true;
    }
    return false;
}

char get_char_from_keycode(size_t keycode)
{
    for (size_t i = 0; i < READABLE_KEYS; i++)
    {
        if (key_codes[i] == keycode)
        {
            return char_matrix[i];
        }
    }
    exit(EINVC);
}

void send_key_to_device(int keyboard_device, struct input_event event)
{
    int write_status = write(keyboard_device, &event, sizeof(event));
    if (write_status < 0)
    {
        printf("Error writing to virtual device\n");
        exit(EWRITE);
    }
}

void send_backspace(int device_fd, size_t n)
{
    struct input_event event;
    event.code = KEY_BACKSPACE;
    event.type = EV_KEY;
    for (size_t i = 0; i < n; i++)
    {
        event.value = 1;
        send_key_to_device(device_fd, event);
        event.value = 0;
        send_key_to_device(device_fd, event);
    }
}

void send_sync(int device_fd)
{
    struct input_event event;
    event.type = EV_SYN;
    event.value = SYN_REPORT;
    send_key_to_device(device_fd, event);
}

void init_trie(struct trie *trie, char character)
{
    trie->character = '\0';
    trie->parent = NULL;
    if (character != NULL)
    {
        trie->character = character;
        trie->keycode = get_keycode_from_char(character);
    }
    trie->is_leaf = false;
    trie->expansion = NULL;
    for (size_t i = 0; i < READABLE_KEYS; i++)
    {
        trie->next[i] = NULL;
    }
}

size_t get_position_from_char(char character)
{
    for (size_t i = 0; i < READABLE_KEYS; i++)
    {
        if (character == char_matrix[i])
        {
            return i;
        }
    }
    exit(EINVCH);
}

size_t get_keycode_from_char(char character)
{
    for (size_t i = 0; i < READABLE_KEYS; i++)
    {
        if (character == char_matrix[i])
        {
            return key_codes[i];
        }
    }
    exit(EINVCH);
}

void push_trie(char *key, char *expansion)
{
    struct trie *current_trie = TRIE;
    for (size_t i = 0; i < strlen(key); i++)
    {
        char character = key[i];
        size_t position = get_position_from_char(character);
        if (current_trie->next[position] == NULL)
        {
            current_trie->next[position] = malloc(sizeof(struct trie));
            init_trie(current_trie->next[position], character);
            current_trie->next[position]->parent = current_trie;
        }
        current_trie = current_trie->next[position];
    }
    current_trie->is_leaf = true;
    current_trie->expansion = malloc(strlen(expansion) + 1);
    strcpy(current_trie->expansion, expansion);
}

void send_to_keyboard(int keyboard_device, char *string)
{
    size_t len = strlen(string);
    struct input_event event;
    event.type = EV_KEY;
    for (size_t i = 0; i < len; i++)
    {
        char character = string[i];
        event.code = get_keycode_from_char(character);
        event.value = 1;
        send_key_to_device(keyboard_device, event);
        event.value = 0;
        send_key_to_device(keyboard_device, event);
    }
}

void start_expanse(int keyboard_device, int vkeyboard_device)
{
    struct input_event event;
    struct trie *current_trie = TRIE;
    while (1)
    {
        int read_inputs = read(keyboard_device, &event, sizeof(struct input_event));
        if (read_inputs < 0)
        {
            printf("Error reading from %s\n", KEYBOARD_DEVICE);
            exit(EREAD);
        }
        if (event.type == EV_KEY && valid_key_code(event.code) && event.value == 1)
        {
            char character = get_char_from_keycode(event.code);
            size_t position = get_position_from_char(character);
            if (current_trie->next[position] != NULL)
            {
                current_trie = current_trie->next[position];
                if (current_trie->is_leaf)
                {
                    size_t key_char_count = 0;
                    struct trie *cursor = current_trie;
                    while (cursor->character != NULL)
                    {
                        key_char_count++;
                        cursor = cursor->parent;
                    }
                    send_backspace(vkeyboard_device, key_char_count);
                    send_to_keyboard(vkeyboard_device, current_trie->expansion);
                    send_sync(vkeyboard_device);
                    current_trie = TRIE;
                }
                continue;
            }
            current_trie = TRIE;
        }
    }
}

void init_virtual_device(int vkeyboard_device)
{
    struct uinput_setup usetup;

    int status;
    // setup as keyboard
    if ((status = ioctl(vkeyboard_device, UI_SET_EVBIT, EV_KEY)) < 0)
    {
        printf("Error initializing virtual input\n");
        exit(EINIT);
    }
    // setup keys to emit
    if ((status = ioctl(vkeyboard_device, UI_SET_KEYBIT, KEY_BACKSPACE)) < 0)
    {
        printf("Error adding key to virtual input : %d\n", KEY_BACKSPACE);
        exit(EADD);
    }
    for (size_t i = 0; i < READABLE_KEYS; i++)
    {
        if ((status = ioctl(vkeyboard_device, UI_SET_KEYBIT, key_codes[i])) < 0)
        {
            printf("Error adding key to virtual input : %d\n", key_codes[i]);
            exit(EADD);
        }
    }

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x7961646176;
    usetup.id.product = 1999;
    strcpy(usetup.name, "keydogger");

    if ((status = ioctl(vkeyboard_device, UI_DEV_SETUP, &usetup)) < 0)
    {
        printf("Error setting up virtual device\n");
        exit(ESETUP);
    }
    if ((status = ioctl(vkeyboard_device, UI_DEV_CREATE)) < 0)
    {
        printf("Error creating up virtual device\n");
        exit(ECREATE);
    };
}

int main()
{
    if (!check_priveleges())
    {
        printf("Need sudo priveleges\n");
        exit(EPERM);
    }
    int fkeyboard_device = open(KEYBOARD_DEVICE, O_RDWR | O_APPEND, NULL);

    if (fkeyboard_device < 0)
    {
        printf("Error opening %s\n", KEYBOARD_DEVICE);
        exit(EOPEN);
    }
    int vkeyboard_device = open(UINPUT_PATH, O_WRONLY);
    ;
    if (open < 0)
    {
        printf("Error reading from %s\n", UINPUT_PATH);
        exit(EOPEN);
    }

    TRIE = malloc(sizeof(struct trie));
    init_trie(TRIE, NULL);
    init_virtual_device(vkeyboard_device);
    read_from_rc();

    start_expanse(fkeyboard_device, vkeyboard_device);
}
