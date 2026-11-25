/*
 * MAC Address Table of a Layer 2 Switch
 *
 * - Hash table with separate chaining (linked list per bucket)
 * - Learns source MAC -> port
 * - Forwards, floods or filters each frame
 * - MAC aging: entries not seen for AGING_TIME seconds are removed
 * - Input validation for MAC addresses and port numbers
 *
 * Build: gcc -std=c99 -Wall -Wextra -o mac_table mac_table_simple.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define TABLE_SIZE   16      /* number of hash buckets            */
#define MAC_STR_LEN  18      /* "AA:BB:CC:DD:EE:FF" + '\0'        */
#define NUM_PORTS    8       /* switch ports are 1 to 8           */
#define AGING_TIME   30      /* seconds (real switches use 300)   */
#define INPUT_LEN    64

/* Return values of learn_mac() */
#define LEARN_NEW      0     /* new MAC added                     */
#define LEARN_UPDATED  1     /* known MAC, same port, timer reset */
#define LEARN_MOVED    2     /* known MAC, now on another port    */
#define LEARN_FAILED  -1     /* memory allocation failed          */

/* One entry (node) in the MAC table */
struct mac_entry
{
    char mac[MAC_STR_LEN];
    int port;
    time_t last_seen;
    struct mac_entry *next;
};

/* Hash table: each bucket is a linked list */
struct mac_entry *table[TABLE_SIZE] = {NULL};


/* ---------------- Input and validation ---------------- */

/* Read one line of input. Returns 1 on success, 0 on end of input. */
int read_line(const char *prompt, char buf[], int size)
{
    int len;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buf, size, stdin) == NULL)
        return 0;

    len = (int)strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
    {
        buf[len - 1] = '\0';          /* remove the newline */
    }
    else
    {
        int c;                        /* line too long: discard the rest */
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        buf[0] = '\0';                /* makes the input invalid */
    }
    return 1;
}

/*
 * Check the MAC format and convert it to standard form.
 * Accepts AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF, any case.
 * Converts it to uppercase with ':' separators.
 * Returns 1 if valid, 0 if not.
 */
int validate_mac(char mac[])
{
    int i;

    if (strlen(mac) != MAC_STR_LEN - 1)
        return 0;

    for (i = 0; i < MAC_STR_LEN - 1; i++)
    {
        if (i % 3 == 2)                       /* positions 2, 5, 8 ... */
        {
            if (mac[i] != ':' && mac[i] != '-')
                return 0;
            mac[i] = ':';
        }
        else
        {
            if (!isxdigit((unsigned char)mac[i]))
                return 0;
            mac[i] = (char)toupper((unsigned char)mac[i]);
        }
    }
    return 1;
}

/* Returns 1 if the MAC is the broadcast address, 0 if not. */
int is_broadcast(const char mac[])
{
    return strcmp(mac, "FF:FF:FF:FF:FF:FF") == 0;
}

/*
 * Check the port is a whole number from 1 to NUM_PORTS.
 * Returns the port number, or -1 if invalid.
 */
int validate_port(const char text[])
{
    char *end;
    long port;

    if (text[0] == '\0')
        return -1;

    port = strtol(text, &end, 10);

    if (*end != '\0' || port < 1 || port > NUM_PORTS)
        return -1;

    return (int)port;
}


/* ---------------- MAC table operations ---------------- */

/* Hash function (djb2): returns a bucket number from 0 to TABLE_SIZE-1 */
unsigned int hash_mac(const char mac[])
{
    unsigned int hash = 5381;
    int i;

    for (i = 0; mac[i] != '\0'; i++)
        hash = hash * 33 + (unsigned char)mac[i];

    return hash % TABLE_SIZE;
}

/*
 * Learn the source MAC on the given port.
 * Returns LEARN_NEW, LEARN_UPDATED, LEARN_MOVED or LEARN_FAILED.
 * If the MAC moved, *old_port is set to its previous port.
 */
int learn_mac(const char mac[], int port, int *old_port)
{
    unsigned int index = hash_mac(mac);
    struct mac_entry *temp = table[index];

    /* Is the MAC already in the table? */
    while (temp != NULL)
    {
        if (strcmp(temp->mac, mac) == 0)
        {
            temp->last_seen = time(NULL);       /* reset aging timer */

            if (temp->port == port)
                return LEARN_UPDATED;

            *old_port = temp->port;             /* device moved */
            temp->port = port;
            return LEARN_MOVED;
        }
        temp = temp->next;
    }

    /* New MAC: create a node and insert it at the head of the list */
    temp = malloc(sizeof(struct mac_entry));
    if (temp == NULL)
        return LEARN_FAILED;

    strcpy(temp->mac, mac);
    temp->port = port;
    temp->last_seen = time(NULL);
    temp->next = table[index];
    table[index] = temp;

    return LEARN_NEW;
}

/* Returns the port of the MAC, or -1 if it is not in the table. */
int find_mac(const char mac[])
{
    struct mac_entry *temp = table[hash_mac(mac)];

    while (temp != NULL)
    {
        if (strcmp(temp->mac, mac) == 0)
            return temp->port;
        temp = temp->next;
    }
    return -1;
}

/*
 * MAC aging: remove entries not seen for AGING_TIME seconds.
 * Returns the number of entries removed.
 */
int age_entries(void)
{
    int i, removed = 0;
    time_t now = time(NULL);

    for (i = 0; i < TABLE_SIZE; i++)
    {
        struct mac_entry *temp = table[i];
        struct mac_entry *prev = NULL;

        while (temp != NULL)
        {
            if (difftime(now, temp->last_seen) >= AGING_TIME)
            {
                struct mac_entry *del = temp;

                printf("  [AGING] %s removed (inactive)\n", temp->mac);

                if (prev == NULL)
                    table[i] = temp->next;      /* first node */
                else
                    prev->next = temp->next;    /* middle or last node */

                temp = temp->next;
                free(del);
                removed++;
            }
            else
            {
                prev = temp;
                temp = temp->next;
            }
        }
    }
    return removed;
}

/* Print the MAC table (void: only displays) */
void display_table(void)
{
    int i, count = 0;
    time_t now = time(NULL);
    struct mac_entry *temp;

    printf("\n  ---------- MAC ADDRESS TABLE ----------\n");
    printf("  %-20s %-6s %s\n", "MAC ADDRESS", "PORT", "AGE(sec)");

    for (i = 0; i < TABLE_SIZE; i++)
    {
        for (temp = table[i]; temp != NULL; temp = temp->next)
        {
            printf("  %-20s %-6d %.0f\n",
                   temp->mac, temp->port, difftime(now, temp->last_seen));
            count++;
        }
    }

    if (count == 0)
        printf("  (empty)\n");

    printf("  Total entries: %d   Aging time: %d sec\n", count, AGING_TIME);
    printf("  ---------------------------------------\n");
}

/* Free all entries. Returns the number of entries freed. */
int free_table(void)
{
    int i, freed = 0;

    for (i = 0; i < TABLE_SIZE; i++)
    {
        struct mac_entry *temp = table[i];

        while (temp != NULL)
        {
            struct mac_entry *del = temp;
            temp = temp->next;
            free(del);
            freed++;
        }
        table[i] = NULL;
    }
    return freed;
}

/* Print the ports a frame is flooded to (void: only displays) */
void flood(int incoming)
{
    int p;

    printf("  Flooding frame to ports:");
    for (p = 1; p <= NUM_PORTS; p++)
    {
        if (p != incoming)
            printf(" %d", p);
    }
    printf("\n");
}


/* ---------------- main ---------------- */

int main(void)
{
    char src[INPUT_LEN], dest[INPUT_LEN], port_text[INPUT_LEN];
    int incoming, outgoing, old_port = 0, result, count;

    printf("=== Switch MAC Address Table (%d ports) ===\n", NUM_PORTS);
    printf("MAC format: AA:BB:CC:DD:EE:FF   Type EXIT to stop\n");

    while (1)
    {
        /* ---- Read and validate source MAC ---- */
        if (!read_line("\nSource MAC      : ", src, INPUT_LEN))
            break;

        if (strcmp(src, "EXIT") == 0 || strcmp(src, "exit") == 0)
            break;

        if (!validate_mac(src))
        {
            printf("  [ERROR] Invalid source MAC. Use format AA:BB:CC:DD:EE:FF\n");
            continue;
        }
        if (is_broadcast(src))
        {
            printf("  [ERROR] Broadcast address cannot be a source MAC\n");
            continue;
        }

        /* ---- Read and validate destination MAC ---- */
        if (!read_line("Destination MAC : ", dest, INPUT_LEN))
            break;

        if (!validate_mac(dest))
        {
            printf("  [ERROR] Invalid destination MAC. Use format AA:BB:CC:DD:EE:FF\n");
            continue;
        }

        /* ---- Read and validate incoming port ---- */
        if (!read_line("Incoming port   : ", port_text, INPUT_LEN))
            break;

        incoming = validate_port(port_text);
        if (incoming == -1)
        {
            printf("  [ERROR] Invalid port. Enter a number from 1 to %d\n", NUM_PORTS);
            continue;
        }

        /* ---- Step 1: remove expired entries ---- */
        count = age_entries();
        if (count > 0)
            printf("  %d entry(s) aged out\n", count);

        /* ---- Step 2: learn the source MAC ---- */
        result = learn_mac(src, incoming, &old_port);

        if (result == LEARN_NEW)
            printf("  Learned %s on port %d\n", src, incoming);
        else if (result == LEARN_UPDATED)
            printf("  %s already known, timer reset\n", src);
        else if (result == LEARN_MOVED)
            printf("  %s moved from port %d to port %d\n", src, old_port, incoming);
        else
            printf("  [WARNING] Out of memory, could not learn %s\n", src);

        /* ---- Step 3: forwarding decision ---- */
        if (is_broadcast(dest))
        {
            printf("  Broadcast frame\n");
            flood(incoming);
        }
        else
        {
            outgoing = find_mac(dest);

            if (outgoing == -1)
            {
                printf("  Destination unknown\n");
                flood(incoming);
            }
            else if (outgoing == incoming)
            {
                printf("  Destination is on the same port, frame discarded\n");
            }
            else
            {
                printf("  Destination found, forwarding frame to port %d\n", outgoing);
            }
        }

        display_table();
    }

    count = free_table();
    printf("\n%d entry(s) freed. Program ended.\n", count);

    return 0;
}
