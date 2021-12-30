#ifndef PARSER_LIB
#define PARSER_LIB
#include "banking.h"

int parse_proc_amount(int argc, char *argv[], int *proc_amount);
int parse_balances(int argc, char* argv[], int proc_amount, balance_t** balances);
int parse_arg(int argc, char *argv[], int *proc_amount, balance_t** balances);


#endif
