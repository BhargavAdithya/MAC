/*
 * mac_table.c - Layer 2 switch MAC address table simulator
 *
 * Features (modelled on real switches):
 *   - Hash table with separate chaining (O(1) average lookup)
 *   - Dynamic learning from the source MAC of each frame
 *   - Aging of dynamic entries (configurable, 0 = disabled)
 *   - Static entries (never age, never overwritten by learning)
 *   - MAC move detection and MAC flapping warning
 *   - Table capacity limit (real switches have a fixed-size CAM)
 *   - Broadcast, multicast and unknown-unicast flooding
 *   - Same-port filtering
 *   - Strict input validation
 *   - Traffic and table statistics
 *
 * Build: gcc -std=c99 -Wall -Wextra -pedantic -o mac_table mac_table.c
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

#define MAC_LEN           6       /* a MAC address is 6 bytes           */
#define MAC_STR_LEN       18      /* "AA:BB:CC:DD:EE:FF" + '\0'         */
#define NUM_PORTS         8       /* switch ports are 1..NUM_PORTS      */
#define HASH_BUCKETS      64      /* number of hash table buckets       */
#define MAX_ENTRIES       256     /* table capacity (like a real CAM)   */
#define DEFAULT_AGING_SEC 300     /* same default as most real switches */
#define MIN_AGING_SEC     10
#define MAX_AGING_SEC     1000000
#define FLAP_WINDOW_SEC   10      /* moves within this window count...  */
#define FLAP_THRESHOLD    3       /* ...and this many means flapping    */
#define INPUT_BUF_LEN     64

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

/* Result codes returned by the table and input functions. */
typedef enum
{
    STATUS_OK = 0,
    STATUS_LEARNED,          /* new MAC added                      */
    STATUS_REFRESHED,        /* known MAC, same port, timer reset  */
    STATUS_MOVED,            /* known MAC seen on a new port       */
    STATUS_FLAPPING,         /* MAC moving between ports too often */
    STATUS_STATIC_PROTECTED, /* static entry not changed           */
    STATUS_EOF,              /* end of input                       */

    /* Everything from here down is an error. */
    STATUS_ERR_NULL,
    STATUS_ERR_INVALID_MAC,
    STATUS_ERR_MULTICAST,
    STATUS_ERR_INVALID_PORT,
    STATUS_ERR_INVALID_AGING,
    STATUS_ERR_TABLE_FULL,
    STATUS_ERR_NO_MEMORY,
    STATUS_ERR_NOT_FOUND,
    STATUS_ERR_INPUT
} status_t;

typedef enum
{
    ENTRY_DYNAMIC,
    ENTRY_STATIC
} entry_type_t;

/* What the switch does with a frame. */
typedef enum
{
    ACTION_FORWARD,  /* send out of one known port        */
    ACTION_FLOOD,    /* send out of all other ports       */
    ACTION_FILTER    /* destination is on incoming port   */
} fwd_action_t;

/* One node of the hash table (one learned MAC). */
typedef struct mac_entry
{
    uint8_t mac[MAC_LEN];
    int port;
    entry_type_t type;
    time_t last_seen;
    time_t last_move;
    unsigned int move_count;
    struct mac_entry *next;
} mac_entry_t;

typedef struct
{
    unsigned long frames;
    unsigned long forwarded;
    unsigned long flooded;
    unsigned long filtered;
    unsigned long dropped;
    unsigned long learned;
    unsigned long moves;
    unsigned long aged_out;
} switch_stats_t;

/* The whole MAC table. No global variables: it is passed by pointer. */
typedef struct
{
    mac_entry_t *buckets[HASH_BUCKETS];
    size_t count;
    unsigned int aging_time;   /* seconds, 0 = aging disabled */
    switch_stats_t stats;
} mac_table_t;

/* ------------------------------------------------------------------ */
/* Status helpers                                                     */
/* ------------------------------------------------------------------ */

static bool is_error(status_t s)
{
    return s >= STATUS_ERR_NULL;
}

static const char *status_to_string(status_t s)
{
    switch (s)
    {
    case STATUS_OK:                return "OK";
    case STATUS_LEARNED:           return "New MAC learned";
    case STATUS_REFRESHED:         return "MAC refreshed";
    case STATUS_MOVED:             return "MAC moved to a new port";
    case STATUS_FLAPPING:          return "MAC flapping between ports";
    case STATUS_STATIC_PROTECTED:  return "Static entry, not changed";
    case STATUS_EOF:               return "End of input";
    case STATUS_ERR_NULL:          return "Internal error (NULL pointer)";
    case STATUS_ERR_INVALID_MAC:   return "Invalid MAC address";
    case STATUS_ERR_MULTICAST:     return "Broadcast/multicast MAC not allowed here "
                                          "(first byte must be even, e.g. AA, 00, 02)";
    case STATUS_ERR_INVALID_PORT:  return "Invalid port number";
    case STATUS_ERR_INVALID_AGING: return "Invalid aging time";
    case STATUS_ERR_TABLE_FULL:    return "MAC table is full";
    case STATUS_ERR_NO_MEMORY:     return "Out of memory";
    case STATUS_ERR_NOT_FOUND:     return "MAC address not found";
    case STATUS_ERR_INPUT:         return "Invalid input";
    }
    return "Unknown status";
}

/* ------------------------------------------------------------------ */
/* MAC address helpers                                                */
/* ------------------------------------------------------------------ */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

/*
 * Convert text to 6 bytes.
 * Accepts AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF, any letter case.
 */
static status_t parse_mac(const char *str, uint8_t mac[MAC_LEN])
{
    uint8_t tmp[MAC_LEN];
    char sep;
    int i;

    if (str == NULL || mac == NULL)
        return STATUS_ERR_NULL;

    if (strlen(str) != MAC_STR_LEN - 1)
        return STATUS_ERR_INVALID_MAC;

    sep = str[2];
    if (sep != ':' && sep != '-')
        return STATUS_ERR_INVALID_MAC;

    for (i = 0; i < MAC_LEN; i++)
    {
        const char *p = str + (i * 3);
        int hi = hex_value(p[0]);
        int lo = hex_value(p[1]);

        if (hi < 0 || lo < 0)
            return STATUS_ERR_INVALID_MAC;

        /* every pair except the last must be followed by the same separator */
        if (i < MAC_LEN - 1 && p[2] != sep)
            return STATUS_ERR_INVALID_MAC;

        tmp[i] = (uint8_t)((hi << 4) | lo);
    }

    memcpy(mac, tmp, MAC_LEN);
    return STATUS_OK;
}

/* Write the MAC as "AA:BB:CC:DD:EE:FF" into buf and return buf. */
static const char *format_mac(const uint8_t mac[MAC_LEN], char buf[MAC_STR_LEN])
{
    snprintf(buf, MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

static bool is_broadcast(const uint8_t mac[MAC_LEN])
{
    int i;
    for (i = 0; i < MAC_LEN; i++)
    {
        if (mac[i] != 0xFF)
            return false;
    }
    return true;
}

/* The lowest bit of the first byte marks a group (multicast) address. */
static bool is_multicast(const uint8_t mac[MAC_LEN])
{
    return (mac[0] & 0x01) != 0;
}

static bool is_zero_mac(const uint8_t mac[MAC_LEN])
{
    int i;
    for (i = 0; i < MAC_LEN; i++)
    {
        if (mac[i] != 0x00)
            return false;
    }
    return true;
}

/* A MAC that can be stored in the table must be a real unicast address. */
static status_t validate_unicast(const uint8_t mac[MAC_LEN])
{
    if (is_zero_mac(mac))
        return STATUS_ERR_INVALID_MAC;
    if (is_multicast(mac))          /* also covers broadcast */
        return STATUS_ERR_MULTICAST;
    return STATUS_OK;
}

static bool is_valid_port(int port)
{
    return port >= 1 && port <= NUM_PORTS;
}

/* djb2 hash over the 6 bytes, reduced to a bucket index. */
static unsigned int hash_mac(const uint8_t mac[MAC_LEN])
{
    unsigned int hash = 5381;
    int i;

    for (i = 0; i < MAC_LEN; i++)
        hash = (hash * 33) + mac[i];

    return hash % HASH_BUCKETS;
}

/* ------------------------------------------------------------------ */
/* MAC table operations                                               */
/* ------------------------------------------------------------------ */

static status_t table_init(mac_table_t *t, unsigned int aging_time)
{
    int i;

    if (t == NULL)
        return STATUS_ERR_NULL;

    for (i = 0; i < HASH_BUCKETS; i++)
        t->buckets[i] = NULL;

    t->count = 0;
    t->aging_time = aging_time;
    memset(&t->stats, 0, sizeof(t->stats));
    return STATUS_OK;
}

/* Return the entry for this MAC, or NULL if it is not in the table. */
static mac_entry_t *table_find(const mac_table_t *t, const uint8_t mac[MAC_LEN])
{
    mac_entry_t *e;

    if (t == NULL || mac == NULL)
        return NULL;

    for (e = t->buckets[hash_mac(mac)]; e != NULL; e = e->next)
    {
        if (memcmp(e->mac, mac, MAC_LEN) == 0)
            return e;
    }
    return NULL;
}

/* Create a new node and insert it at the head of its bucket. */
static status_t table_insert(mac_table_t *t, const uint8_t mac[MAC_LEN],
                             int port, entry_type_t type, time_t now)
{
    unsigned int index;
    mac_entry_t *e;

    if (t->count >= MAX_ENTRIES)
        return STATUS_ERR_TABLE_FULL;

    e = malloc(sizeof(*e));
    if (e == NULL)
        return STATUS_ERR_NO_MEMORY;

    memcpy(e->mac, mac, MAC_LEN);
    e->port = port;
    e->type = type;
    e->last_seen = now;
    e->last_move = 0;
    e->move_count = 0;

    index = hash_mac(mac);
    e->next = t->buckets[index];
    t->buckets[index] = e;
    t->count++;
    return STATUS_OK;
}

/*
 * Learn the source MAC of a frame.
 * If the MAC moved, *old_port is set to the port it was on before.
 */
static status_t table_learn(mac_table_t *t, const uint8_t mac[MAC_LEN],
                            int port, time_t now, int *old_port)
{
    mac_entry_t *e;
    status_t s;

    if (t == NULL || mac == NULL)
        return STATUS_ERR_NULL;
    if (!is_valid_port(port))
        return STATUS_ERR_INVALID_PORT;

    s = validate_unicast(mac);
    if (s != STATUS_OK)
        return s;

    e = table_find(t, mac);

    if (e == NULL)
    {
        s = table_insert(t, mac, port, ENTRY_DYNAMIC, now);
        if (s != STATUS_OK)
            return s;
        t->stats.learned++;
        return STATUS_LEARNED;
    }

    if (e->type == ENTRY_STATIC)
        return (e->port == port) ? STATUS_REFRESHED : STATUS_STATIC_PROTECTED;

    e->last_seen = now;

    if (e->port == port)
        return STATUS_REFRESHED;

    /* The MAC moved. Count moves that happen close together. */
    if (old_port != NULL)
        *old_port = e->port;

    if (e->last_move != 0 && difftime(now, e->last_move) <= FLAP_WINDOW_SEC)
        e->move_count++;
    else
        e->move_count = 1;

    e->last_move = now;
    e->port = port;
    t->stats.moves++;

    return (e->move_count >= FLAP_THRESHOLD) ? STATUS_FLAPPING : STATUS_MOVED;
}

/* Add a static entry, or turn an existing dynamic entry into a static one. */
static status_t table_add_static(mac_table_t *t, const uint8_t mac[MAC_LEN],
                                 int port, time_t now)
{
    mac_entry_t *e;
    status_t s;

    if (t == NULL || mac == NULL)
        return STATUS_ERR_NULL;
    if (!is_valid_port(port))
        return STATUS_ERR_INVALID_PORT;

    s = validate_unicast(mac);
    if (s != STATUS_OK)
        return s;

    e = table_find(t, mac);
    if (e != NULL)
    {
        e->type = ENTRY_STATIC;
        e->port = port;
        e->last_seen = now;
        return STATUS_OK;
    }

    return table_insert(t, mac, port, ENTRY_STATIC, now);
}

/* Remove one entry (static or dynamic). */
static status_t table_delete(mac_table_t *t, const uint8_t mac[MAC_LEN])
{
    unsigned int index;
    mac_entry_t *e;
    mac_entry_t *prev = NULL;

    if (t == NULL || mac == NULL)
        return STATUS_ERR_NULL;

    index = hash_mac(mac);

    for (e = t->buckets[index]; e != NULL; prev = e, e = e->next)
    {
        if (memcmp(e->mac, mac, MAC_LEN) == 0)
        {
            if (prev == NULL)
                t->buckets[index] = e->next;
            else
                prev->next = e->next;

            free(e);
            t->count--;
            return STATUS_OK;
        }
    }
    return STATUS_ERR_NOT_FOUND;
}

/*
 * Remove entries that match the rule:
 *   only_expired = true  -> dynamic entries older than the aging time
 *   only_expired = false -> all dynamic entries
 * Static entries are never removed here. Returns how many were removed.
 */
static size_t remove_dynamic(mac_table_t *t, bool only_expired, time_t now)
{
    size_t removed = 0;
    int i;

    for (i = 0; i < HASH_BUCKETS; i++)
    {
        mac_entry_t *e = t->buckets[i];
        mac_entry_t *prev = NULL;

        while (e != NULL)
        {
            bool expired = difftime(now, e->last_seen) >= (double)t->aging_time;
            bool remove = (e->type == ENTRY_DYNAMIC) && (!only_expired || expired);

            if (remove)
            {
                mac_entry_t *del = e;

                if (prev == NULL)
                    t->buckets[i] = e->next;
                else
                    prev->next = e->next;

                e = e->next;
                free(del);
                t->count--;
                removed++;
            }
            else
            {
                prev = e;
                e = e->next;
            }
        }
    }
    return removed;
}

/* Remove expired dynamic entries. Returns how many were removed. */
static size_t table_age(mac_table_t *t, time_t now)
{
    size_t removed;

    if (t == NULL || t->aging_time == 0)
        return 0;

    removed = remove_dynamic(t, true, now);
    t->stats.aged_out += removed;
    return removed;
}

/* Remove all dynamic entries. Returns how many were removed. */
static size_t table_clear_dynamic(mac_table_t *t)
{
    if (t == NULL)
        return 0;
    return remove_dynamic(t, false, time(NULL));
}

static status_t table_set_aging(mac_table_t *t, long seconds)
{
    if (t == NULL)
        return STATUS_ERR_NULL;

    if (seconds != 0 && (seconds < MIN_AGING_SEC || seconds > MAX_AGING_SEC))
        return STATUS_ERR_INVALID_AGING;

    t->aging_time = (unsigned int)seconds;
    return STATUS_OK;
}

/* Free every entry. Returns how many were freed. */
static size_t table_destroy(mac_table_t *t)
{
    size_t freed = 0;
    int i;

    if (t == NULL)
        return 0;

    for (i = 0; i < HASH_BUCKETS; i++)
    {
        mac_entry_t *e = t->buckets[i];

        while (e != NULL)
        {
            mac_entry_t *next = e->next;
            free(e);
            freed++;
            e = next;
        }
        t->buckets[i] = NULL;
    }
    t->count = 0;
    return freed;
}

/* Decide what to do with a frame. Sets *out_port when forwarding. */
static fwd_action_t decide_forwarding(const mac_table_t *t,
                                      const uint8_t dest[MAC_LEN],
                                      int in_port, int *out_port)
{
    const mac_entry_t *e;

    if (is_multicast(dest))         /* broadcast or multicast */
        return ACTION_FLOOD;

    e = table_find(t, dest);
    if (e == NULL)                  /* unknown unicast */
        return ACTION_FLOOD;

    if (e->port == in_port)
        return ACTION_FILTER;

    *out_port = e->port;
    return ACTION_FORWARD;
}

/* ------------------------------------------------------------------ */
/* Input helpers                                                      */
/* ------------------------------------------------------------------ */

/* Remove spaces at both ends of a string, in place. */
static char *trim(char *s)
{
    char *start = s;
    char *end;

    while (isspace((unsigned char)*start))
        start++;

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';

    if (start != s)
        memmove(s, start, (size_t)(end - start) + 1);
    return s;
}

/* Read one line. Rejects lines that are too long. */
static status_t read_line(const char *prompt, char *buf, size_t size)
{
    size_t len;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buf, (int)size, stdin) == NULL)
        return STATUS_EOF;

    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
    {
        buf[len - 1] = '\0';
    }
    else if (!feof(stdin))
    {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;                       /* discard the rest of the long line */
        return STATUS_ERR_INPUT;
    }

    trim(buf);
    return STATUS_OK;
}

/* Keep asking until a valid MAC is entered. */
static status_t prompt_mac(const char *prompt, uint8_t mac[MAC_LEN])
{
    char buf[INPUT_BUF_LEN];
    status_t s;

    for (;;)
    {
        s = read_line(prompt, buf, sizeof(buf));
        if (s == STATUS_EOF)
            return s;

        if (s == STATUS_OK)
        {
            s = parse_mac(buf, mac);
            if (s == STATUS_OK)
                return s;
        }
        printf("  [!] Invalid MAC. Use format AA:BB:CC:DD:EE:FF\n");
    }
}

/* Keep asking until a whole number between min and max is entered. */
static status_t prompt_long(const char *prompt, long min, long max, long *out)
{
    char buf[INPUT_BUF_LEN];
    char *end;
    long value;
    status_t s;

    for (;;)
    {
        s = read_line(prompt, buf, sizeof(buf));
        if (s == STATUS_EOF)
            return s;

        if (s == STATUS_OK && buf[0] != '\0')
        {
            errno = 0;
            value = strtol(buf, &end, 10);

            if (errno == 0 && *end == '\0' && value >= min && value <= max)
            {
                *out = value;
                return STATUS_OK;
            }
        }
        printf("  [!] Enter a whole number from %ld to %ld\n", min, max);
    }
}

static status_t prompt_port(const char *prompt, int *port)
{
    long value;
    status_t s = prompt_long(prompt, 1, NUM_PORTS, &value);

    if (s == STATUS_OK)
        *port = (int)value;
    return s;
}

/* ------------------------------------------------------------------ */
/* Display functions (void: they only print)                          */
/* ------------------------------------------------------------------ */

static void print_menu(void)
{
    printf("\n=========== SWITCH MENU ===========\n");
    printf(" 1. Send a frame\n");
    printf(" 2. Show MAC address table\n");
    printf(" 3. Look up a MAC address\n");
    printf(" 4. Add static entry\n");
    printf(" 5. Delete an entry\n");
    printf(" 6. Clear dynamic entries\n");
    printf(" 7. Set aging time\n");
    printf(" 8. Show statistics\n");
    printf(" 0. Exit\n");
    printf("===================================\n");
}

static void print_entry(const mac_entry_t *e, time_t now)
{
    char buf[MAC_STR_LEN];

    if (e->type == ENTRY_STATIC)
        printf("  %-19s %-9s %-6d %s\n",
               format_mac(e->mac, buf), "STATIC", e->port, "-");
    else
        printf("  %-19s %-9s %-6d %.0f\n",
               format_mac(e->mac, buf), "DYNAMIC", e->port,
               difftime(now, e->last_seen));
}

static void show_table(const mac_table_t *t)
{
    time_t now = time(NULL);
    unsigned long statics = 0;
    int i;

    printf("\n            MAC ADDRESS TABLE\n");
    printf("  %-19s %-9s %-6s %s\n", "MAC Address", "Type", "Port", "Age(s)");
    printf("  ---------------------------------------------\n");

    for (i = 0; i < HASH_BUCKETS; i++)
    {
        const mac_entry_t *e;
        for (e = t->buckets[i]; e != NULL; e = e->next)
        {
            print_entry(e, now);
            if (e->type == ENTRY_STATIC)
                statics++;
        }
    }

    if (t->count == 0)
        printf("  (empty)\n");

    printf("  ---------------------------------------------\n");
    printf("  Total: %lu/%d  (static %lu, dynamic %lu)\n",
           (unsigned long)t->count, MAX_ENTRIES,
           statics, (unsigned long)t->count - statics);

    if (t->aging_time == 0)
        printf("  Aging: disabled\n");
    else
        printf("  Aging time: %u s\n", t->aging_time);
}

static void show_stats(const mac_table_t *t)
{
    const switch_stats_t *s = &t->stats;

    printf("\n          SWITCH STATISTICS\n");
    printf("  Frames received   : %lu\n", s->frames);
    printf("  Forwarded         : %lu\n", s->forwarded);
    printf("  Flooded           : %lu\n", s->flooded);
    printf("  Filtered          : %lu\n", s->filtered);
    printf("  Dropped (invalid) : %lu\n", s->dropped);
    printf("  MACs learned      : %lu\n", s->learned);
    printf("  MAC moves         : %lu\n", s->moves);
    printf("  Aged out          : %lu\n", s->aged_out);
}

static void print_flood_ports(int in_port)
{
    int p;

    printf("  -> Flooding to ports:");
    for (p = 1; p <= NUM_PORTS; p++)
    {
        if (p != in_port)
            printf(" %d", p);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Menu commands: each returns a status that main() checks            */
/* ------------------------------------------------------------------ */

static status_t cmd_send_frame(mac_table_t *t)
{
    uint8_t src[MAC_LEN], dest[MAC_LEN];
    char buf[MAC_STR_LEN];
    int in_port, out_port = 0, old_port = 0;
    status_t s;
    fwd_action_t action;

    if ((s = prompt_mac("Source MAC      : ", src)) != STATUS_OK)
        return s;
    if ((s = prompt_mac("Destination MAC : ", dest)) != STATUS_OK)
        return s;
    if ((s = prompt_port("Incoming port   : ", &in_port)) != STATUS_OK)
        return s;

    t->stats.frames++;

    /* Step 1: learn the source MAC. */
    s = table_learn(t, src, in_port, time(NULL), &old_port);

    switch (s)
    {
    case STATUS_LEARNED:
        printf("  Learned %s on port %d\n", format_mac(src, buf), in_port);
        break;
    case STATUS_REFRESHED:
        printf("  %s already known, timer refreshed\n", format_mac(src, buf));
        break;
    case STATUS_MOVED:
        printf("  %s moved from port %d to port %d\n",
               format_mac(src, buf), old_port, in_port);
        break;
    case STATUS_FLAPPING:
        printf("  [WARNING] MAC flapping: %s moved from port %d to port %d "
               "(%d+ moves in %d s). Check for a loop or a duplicate MAC.\n",
               format_mac(src, buf), old_port, in_port,
               FLAP_THRESHOLD, FLAP_WINDOW_SEC);
        break;
    case STATUS_STATIC_PROTECTED:
        printf("  [WARNING] %s is a static entry on another port; not updated\n",
               format_mac(src, buf));
        break;
    case STATUS_ERR_TABLE_FULL:
    case STATUS_ERR_NO_MEMORY:
        /* A real switch still forwards the frame; it just cannot learn. */
        printf("  [WARNING] Could not learn source: %s\n", status_to_string(s));
        break;
    default:
        /* Invalid source (zero / multicast / broadcast): drop the frame. */
        t->stats.dropped++;
        printf("  Frame dropped: source MAC must be a unicast address\n");
        return s;
    }

    /* Step 2: decide where the frame goes. */
    action = decide_forwarding(t, dest, in_port, &out_port);

    switch (action)
    {
    case ACTION_FORWARD:
        t->stats.forwarded++;
        printf("  -> Forwarding to port %d\n", out_port);
        break;
    case ACTION_FLOOD:
        t->stats.flooded++;
        if (is_broadcast(dest))
            printf("  Broadcast frame\n");
        else if (is_multicast(dest))
            printf("  Multicast frame\n");
        else
            printf("  Destination unknown\n");
        print_flood_ports(in_port);
        break;
    case ACTION_FILTER:
        t->stats.filtered++;
        printf("  Destination is on the incoming port -> frame filtered\n");
        break;
    }

    return STATUS_OK;
}

static status_t cmd_lookup(const mac_table_t *t)
{
    uint8_t mac[MAC_LEN];
    const mac_entry_t *e;
    status_t s;

    if ((s = prompt_mac("MAC to look up  : ", mac)) != STATUS_OK)
        return s;

    e = table_find(t, mac);
    if (e == NULL)
        return STATUS_ERR_NOT_FOUND;

    printf("  %-19s %-9s %-6s %s\n", "MAC Address", "Type", "Port", "Age(s)");
    print_entry(e, time(NULL));
    return STATUS_OK;
}

static status_t cmd_add_static(mac_table_t *t)
{
    uint8_t mac[MAC_LEN];
    char buf[MAC_STR_LEN];
    int port;
    status_t s;

    if ((s = prompt_mac("Static MAC      : ", mac)) != STATUS_OK)
        return s;
    if ((s = prompt_port("Port            : ", &port)) != STATUS_OK)
        return s;

    s = table_add_static(t, mac, port, time(NULL));
    if (s == STATUS_OK)
        printf("  Static entry %s -> port %d added\n", format_mac(mac, buf), port);
    return s;
}

static status_t cmd_delete(mac_table_t *t)
{
    uint8_t mac[MAC_LEN];
    char buf[MAC_STR_LEN];
    status_t s;

    if ((s = prompt_mac("MAC to delete   : ", mac)) != STATUS_OK)
        return s;

    s = table_delete(t, mac);
    if (s == STATUS_OK)
        printf("  %s deleted\n", format_mac(mac, buf));
    return s;
}

static status_t cmd_clear_dynamic(mac_table_t *t)
{
    size_t removed = table_clear_dynamic(t);

    printf("  %lu dynamic entries cleared\n", (unsigned long)removed);
    return STATUS_OK;
}

static status_t cmd_set_aging(mac_table_t *t)
{
    long seconds;
    status_t s;

    printf("  Aging time: 0 to disable, or %d to %d seconds\n",
           MIN_AGING_SEC, MAX_AGING_SEC);

    if ((s = prompt_long("New aging time  : ", 0, MAX_AGING_SEC, &seconds)) != STATUS_OK)
        return s;

    s = table_set_aging(t, seconds);
    if (s == STATUS_OK)
    {
        if (seconds == 0)
            printf("  Aging disabled\n");
        else
            printf("  Aging time set to %ld s\n", seconds);
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    mac_table_t table;
    status_t status;
    long choice;
    bool running = true;
    size_t count;

    status = table_init(&table, DEFAULT_AGING_SEC);
    if (status != STATUS_OK)
    {
        fprintf(stderr, "Failed to initialise MAC table: %s\n",
                status_to_string(status));
        return EXIT_FAILURE;
    }

    printf("Layer 2 Switch Simulator (%d ports, capacity %d MACs)\n",
           NUM_PORTS, MAX_ENTRIES);

    while (running)
    {
        print_menu();
        status = prompt_long("Choice: ", 0, 8, &choice);
        if (status == STATUS_EOF)
            break;

        /* Age out old entries before running the chosen command. */
        count = table_age(&table, time(NULL));
        if (count > 0)
            printf("  [INFO] %lu dynamic entries aged out\n", (unsigned long)count);

        switch (choice)
        {
        case 1:  status = cmd_send_frame(&table);    break;
        case 2:  show_table(&table); status = STATUS_OK; break;
        case 3:  status = cmd_lookup(&table);        break;
        case 4:  status = cmd_add_static(&table);    break;
        case 5:  status = cmd_delete(&table);        break;
        case 6:  status = cmd_clear_dynamic(&table); break;
        case 7:  status = cmd_set_aging(&table);     break;
        case 8:  show_stats(&table); status = STATUS_OK; break;
        default: running = false; status = STATUS_OK; break;   /* 0 = exit */
        }

        /* Process the result of the command. */
        if (status == STATUS_EOF)
            running = false;
        else if (is_error(status))
            printf("  [ERROR] %s\n", status_to_string(status));
    }

    count = table_destroy(&table);
    printf("\nFreed %lu entries. Goodbye.\n", (unsigned long)count);
    return EXIT_SUCCESS;
}
