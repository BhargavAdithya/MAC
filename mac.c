#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define TABLE_SIZE 16
#define AGING_TIME 30

// MAC table entry
struct mac_entry
{
    char mac[18];
    int port;
    time_t last_seen;
    struct mac_entry *next;
};

// Hash table
struct mac_entry *table[TABLE_SIZE] = {NULL};

// Convert MAC address to uppercase
void normalize_mac(char mac[])
{
    int i;

    for (i = 0; mac[i] != '\0'; i++)
    {
        mac[i] = toupper((unsigned char)mac[i]);
    }
}

// Hash function
unsigned int hash_mac(char mac[])
{
    unsigned int hash = 5381;
    int i;

    for (i = 0; mac[i] != '\0'; i++)
    {
        hash = ((hash * 33) + mac[i]) % TABLE_SIZE;
    }

    return hash;
}

// Learn or update MAC address
void learn_mac(char mac[], int port)
{
    unsigned int index;
    struct mac_entry *temp;

    index = hash_mac(mac);

    temp = table[index];

    // Check existing MAC
    while (temp != NULL)
    {
        if (strcmp(temp->mac, mac) == 0)
        {
            temp->port = port;
            temp->last_seen = time(NULL);

            printf("MAC address updated!\n");
            return;
        }

        temp = temp->next;
    }

    // Create new entry
    temp = malloc(sizeof(struct mac_entry));

    if (temp == NULL)
    {
        printf("Memory allocation failed!\n");
        return;
    }

    strcpy(temp->mac, mac);
    temp->port = port;
    temp->last_seen = time(NULL);

    // Insert at beginning of linked list
    temp->next = table[index];
    table[index] = temp;

    printf("New MAC address learned!\n");
}

// Search destination MAC
int find_mac(char mac[])
{
    unsigned int index;
    struct mac_entry *temp;

    index = hash_mac(mac);

    temp = table[index];

    while (temp != NULL)
    {
        if (strcmp(temp->mac, mac) == 0)
        {
            return temp->port;
        }

        temp = temp->next;
    }

    return -1;
}

// Remove inactive MAC addresses
void age_entries()
{
    int i;
    time_t current_time;
    struct mac_entry *temp, *prev;

    current_time = time(NULL);

    for (i = 0; i < TABLE_SIZE; i++)
    {
        temp = table[i];
        prev = NULL;

        while (temp != NULL)
        {
            if (difftime(current_time, temp->last_seen) >= AGING_TIME)
            {
                printf("Aging out MAC: %s\n", temp->mac);

                if (prev == NULL)
                    table[i] = temp->next;
                else
                    prev->next = temp->next;

                struct mac_entry *del = temp;
                temp = temp->next;
                free(del);
            }
            else
            {
                prev = temp;
                temp = temp->next;
            }
        }
    }
}

// Display MAC table
void display_table()
{
    int i;
    struct mac_entry *temp;

    printf("\n----- MAC ADDRESS TABLE -----\n");
    printf("%-22s %-10s %-10s\n",
           "MAC ADDRESS", "PORT", "AGE(sec)");

    for (i = 0; i < TABLE_SIZE; i++)
    {
        temp = table[i];

        while (temp != NULL)
        {
            printf("%-22s %-10d %-10.0f\n",
                   temp->mac,
                   temp->port,
                   difftime(time(NULL), temp->last_seen));

            temp = temp->next;
        }
    }

    printf("-----------------------------\n");
}

// Free allocated memory
void free_table()
{
    int i;
    struct mac_entry *temp, *del;

    for (i = 0; i < TABLE_SIZE; i++)
    {
        temp = table[i];

        while (temp != NULL)
        {
            del = temp;
            temp = temp->next;
            free(del);
        }

        table[i] = NULL;
    }
}

int main()
{
    char src[18], dest[18];
    int incoming, outgoing;

    while (1)
    {
        // Remove expired entries
        age_entries();

        printf("\nEnter Source MAC (EXIT to stop): ");
        scanf("%17s", src);

        if (strcmp(src, "EXIT") == 0)
            break;

        printf("Enter Destination MAC: ");
        scanf("%17s", dest);

        printf("Enter Incoming Port: ");
        if (scanf("%d", &incoming) != 1 || incoming <= 0)
        {
            printf("Invalid port number!\n");
            free_table();
            return 1;
        }

        normalize_mac(src);
        normalize_mac(dest);

        // Learn source MAC
        learn_mac(src, incoming);

        // Search destination MAC
        outgoing = find_mac(dest);

        // Forwarding decision
        if (outgoing == -1)
        {
            printf("Destination unknown!\n");
            printf("Flooding frame to other ports.\n");
        }
        else if (outgoing == incoming)
        {
            printf("Destination is on same port.\n");
            printf("Frame discarded.\n");
        }
        else
        {
            printf("Destination found!\n");
            printf("Forwarding frame to Port %d\n", outgoing);
        }

        display_table();
    }

    free_table();

    printf("MAC table memory released.\n");

    return 0;
}
