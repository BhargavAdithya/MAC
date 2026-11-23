#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ENTRIES 10   /* size of the MAC table */
#define NUM_PORTS   4    /* switch has ports 1 to 4 */

struct mac_entry
{
    char mac[18];        /* "AA:BB:CC:DD:EE:FF" = 17 chars + '\0' */
    int port;
};

struct mac_entry table[MAX_ENTRIES];
int count = 0;

/* Make the MAC uppercase so "aa:bb:.." and "AA:BB:.." are treated the same */
void to_upper(char s[])
{
    int i;
    for (i = 0; s[i] != '\0'; i++)
        s[i] = toupper((unsigned char)s[i]);
}

/* Check the MAC is in the format XX:XX:XX:XX:XX:XX (X = hex digit) */
int valid_mac(char s[])
{
    int i;

    if (strlen(s) != 17)
        return 0;

    for (i = 0; i < 17; i++)
    {
        if (i % 3 == 2)
        {
            if (s[i] != ':')
                return 0;
        }
        else if (!isxdigit((unsigned char)s[i]))
            return 0;
    }
    return 1;
}

/* Throw away the rest of the line after bad input */
void clear_input()
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

/* Keep asking until the user types a valid MAC */
void read_mac(char prompt[], char mac[])
{
    char buf[64];

    while (1)
    {
        printf("%s", prompt);
        if (scanf("%63s", buf) != 1)
            exit(0);                     /* end of input */

        to_upper(buf);
        if (valid_mac(buf))
        {
            strcpy(mac, buf);
            return;
        }
        printf("  Invalid MAC. Use format AA:BB:CC:DD:EE:FF\n");
    }
}

/* Keep asking until the user types a port between 1 and NUM_PORTS */
int read_port(char prompt[])
{
    int port;
    int result;

    while (1)
    {
        printf("%s", prompt);
        result = scanf("%d", &port);

        if (result == EOF)
            exit(0);
        if (result == 1 && port >= 1 && port <= NUM_PORTS)
            return port;

        printf("  Invalid port. Enter a number from 1 to %d\n", NUM_PORTS);
        if (result != 1)
            clear_input();
    }
}

/* Learn (or update) which port the source MAC is on */
void learn_mac(char mac[], int port)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (strcmp(table[i].mac, mac) == 0)
        {
            if (table[i].port != port)
                printf("  %s moved from Port %d to Port %d\n",
                       mac, table[i].port, port);
            table[i].port = port;
            return;
        }
    }

    if (count == MAX_ENTRIES)
    {
        printf("  MAC table full! Could not learn %s\n", mac);
        return;
    }

    strcpy(table[count].mac, mac);
    table[count].port = port;
    count++;
    printf("  Learned %s on Port %d\n", mac, port);
}

/* Return the port of a MAC, or -1 if it is not in the table */
int find_mac(char mac[])
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (strcmp(table[i].mac, mac) == 0)
            return table[i].port;
    }
    return -1;
}

/* Send the frame out of every port except the one it came in on */
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

void show_table()
{
    int i;

    printf("\n  ---- MAC Table (%d/%d) ----\n", count, MAX_ENTRIES);
    if (count == 0)
        printf("  (empty)\n");
    for (i = 0; i < count; i++)
        printf("  %-17s  Port %d\n", table[i].mac, table[i].port);
    printf("  --------------------------\n\n");
}

int main()
{
    char src[18], dest[18];
    char choice;
    int incoming, outgoing;

    printf("=== Simple Switch Simulator (%d ports) ===\n\n", NUM_PORTS);

    do
    {
        read_mac("Enter Source MAC: ", src);
        read_mac("Enter Destination MAC: ", dest);
        incoming = read_port("Enter Incoming Port: ");

        printf("\n");
        learn_mac(src, incoming);

        if (strcmp(dest, "FF:FF:FF:FF:FF:FF") == 0)
        {
            printf("  Broadcast frame.\n");
            flood(incoming);
        }
        else
        {
            outgoing = find_mac(dest);

            if (outgoing == -1)
            {
                printf("  Destination unknown!\n");
                flood(incoming);
            }
            else if (outgoing == incoming)
            {
                printf("  Destination is on same port.\n");
                printf("  Frame discarded.\n");
            }
            else
            {
                printf("  Destination found!\n");
                printf("  Forwarding frame to Port %d\n", outgoing);
            }
        }

        show_table();

        printf("Send another frame? (y/n): ");
        if (scanf(" %c", &choice) != 1)
            break;
        printf("\n");

    } while (choice == 'y' || choice == 'Y');

    printf("Exiting.\n");
    return 0;
}
