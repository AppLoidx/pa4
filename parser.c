#include <getopt.h>
#include <stdlib.h>

#include "parser.h"

int parse_proc_amount(int argc, char *argv[], int *proc_amount)
{
    int opt;

    while ((opt = getopt(argc, argv, "p:")) > 0)
    {
        if (opt == 'p')
        {
            *proc_amount = atoi(optarg) + 1;
            return 0;
        }
    }

    return -1;
}

int parse_balances(int argc, char *argv[], int proc_amount, balance_t** balances) {
    int child_proc_amount = proc_amount - 1;
    *balances = malloc(sizeof(balance_t) * child_proc_amount);
    for (int i = 3, j = 0; i < child_proc_amount + 3; i++, j++) {
        (*balances)[j] = atoi(argv[i]);
    }

    return 0;
}

int parse_arg(int argc, char *argv[], int *proc_amount, balance_t** balances) {
    int res = parse_proc_amount(argc, argv, proc_amount);
    if (res < 0) return res;

    return parse_balances(argc, argv, *proc_amount, balances);

}
