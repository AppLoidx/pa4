
#include <sys/types.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <wait.h>
#include <stdlib.h>
#include <errno.h>
#include <sched.h>
#include <assert.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include "ipc.h"
#include "common.h"
#include "pa2345.h"
#include "banking.h"
#include "parser.h"
#include "subheader.h"

#define PARENT_ID 0 // defined in task
#define DEBUG 1

#define PIPE_OPENED_F "Opened pipe: %d -> %d\n"
#define PIPE_CLOSED_F "Closed pipe: %d -> %d\n"
#define MAX_PROC 10
#define CS_QUEUE_SIZE MAX_PROC + 1

timestamp_t lamport_time = 0;

typedef struct
{
    int in;
    int out;
} pipe_io;

// CS QUEUE

typedef struct
{
    size_t amount;
    struct
    {
        local_id id;
        timestamp_t timestamp;
    } data[CS_QUEUE_SIZE];
} cs_queue;

local_id cs_queue_find_index_of_min(cs_queue cs_queue)
{
    local_id min_id = MAX_PROCESS_ID;
    timestamp_t min_time = INT16_MAX;
    int index;

    for (local_id i = 0; i < cs_queue.amount; i++)
    {
        index = i;
        if (min_time < cs_queue.data[i].timestamp)
        {
            continue;
        }

        if (min_time == cs_queue.data[i].timestamp &&
            min_id < cs_queue.data[i].id)
        {
            continue;
        }

        min_time = cs_queue.data[i].timestamp;
        min_id = cs_queue.data[i].id;
    }

    return index;
}

void cs_queue_add_index(cs_queue cs_queue, local_id id, timestamp_t t)
{
    cs_queue.data[cs_queue.amount].id = id;
    cs_queue.data[cs_queue.amount].timestamp = t;
    cs_queue.amount++;
}

local_id cs_queue_pop(cs_queue cs_queue)
{
    local_id id = cs_queue_find_index_of_min(cs_queue);
    cs_queue.amount--;
    cs_queue.data[id] = cs_queue.data[cs_queue.amount]; // replace min elem

    return id;
}

// ----------------

typedef struct
{
    local_id local_id;
    pipe_io *pipes;

    size_t proc_amount;
    pid_t parent_pid;

    FILE *event_fd;
    balance_t balance;

    // CS Logic
    int use_mutex;
    cs_queue cs_queue;

    struct
    {
        size_t started;
        size_t done;
        size_t replies;
    } context;

} proc_data;

// Utility methods
// pos calc functions source, target - local_id
int pipe_pos_count_reader(int source, int target, int n)
{
    return target * (n - 1) + source - (source > target ? 1 : 0);
}

int pipe_pos_count_writer(int source, int target, int n)
{
    return source * (n - 1) + target - (target > source ? 1 : 0);
}

FILE *open_pipe_logfile()
{
    return fopen(pipes_log, "a+");
}

FILE *open_event_logfile()
{
    return fopen(events_log, "a+");
}

int close_file(FILE *file)
{
    return fclose(file);
}

void flogger(FILE *__stream, const char *__fmt, ...)
{
    va_list args;
    va_start(args, __fmt);

    vfprintf(__stream, __fmt, args);
    if (DEBUG == 1)
    {
        va_start(args, __fmt); // call twice if DEBUG enabled
        vprintf(__fmt, args);
    }
}

void vflogger(FILE *__stream, const char *__fmt, va_list args)
{

    vfprintf(__stream, __fmt, args);
}

void logger(const char *__fmt, ...)
{
    va_list args;
    va_start(args, __fmt);
    if (DEBUG == 1)
    {
        vprintf(__fmt, args);
    }
}

void pipe_opened_log(int ifd, int ofd)
{
    FILE *pipe_log = open_pipe_logfile();

    // flogger(pipe_log, PIPE_OPENED_F, ifd, ofd);

    close_file(pipe_log);
}

void event_log(const char *__fmt, ...)
{
    va_list args;
    va_start(args, __fmt);
    FILE *file = open_event_logfile();
    vflogger(file, __fmt, args);

    if (DEBUG == 1)
    {
        va_start(args, __fmt); // call twice if DEBUG enabled
        vprintf(__fmt, args);
    }

    close_file(file);
}

// make fd non-block
int nonblock(int fd)
{
    const int flag = fcntl(fd, F_GETFL);

    if (flag == -1)
    {
        return flag;
    }

    if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) != 0)
    {
        return -2;
    }

    return 0;
}

int receive_block(proc_data *proc_data, local_id id, Message *msg)
{
    // for working with blocking IO
    while (true)
    {
        const int ret = receive((void *)proc_data, id, msg);

        if (ret != 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                // if he is temp unavailable we should loop again and wait
                continue;
            }
        }

        return ret; // success or another error
    }
}

bool create_pipe(pipe_io *pipe_io)
{
    int fds[2];

    int res = pipe(fds);

    if (res < 0)
    {
        puts("Error while creating pipe");
        return 0;
    }
    else
    {
    }

    pipe_opened_log(fds[0], fds[1]);
    nonblock(fds[0]);
    nonblock(fds[1]);

    pipe_io->in = fds[0];
    pipe_io->out = fds[1];

    return 1;
}

void create_pipes(int amount, pipe_io *__pipes)
{

    for (int i = 0; i < amount; i++)
    {
        create_pipe(&__pipes[i]);
    }
}

bool close_pipe(pipe_io pipe_io)
{

    int r1 = close(pipe_io.in);
    r1 += close(pipe_io.out);

    if (r1 == 0)
    {
        FILE *pipe_log = open_pipe_logfile();
        // flogger(pipe_log, PIPE_CLOSED_F, pipe_io.in, pipe_io.out);
        close_file(pipe_log);
    }
    else
    {
        logger("Cannot close pipes : %d -> %d", pipe_io.in, pipe_io.out);
    }

    return r1 == 0;
}

// debug methods

void debug_pipe_io(pipe_io pipe_io)
{
    if (DEBUG == 1)
        printf("[LOG] PIPE IO: in = %d, out = %d\n", pipe_io.in, pipe_io.out);
}

timestamp_t get_lamport_time()
{
    return lamport_time;
}

void set_lamport_time(const Message *msg, Message *new_msg)
{
    lamport_time++;

    new_msg->s_header = msg->s_header;
    new_msg->s_header.s_local_time = lamport_time;

    // copy payload
    memcpy(new_msg->s_payload, msg->s_payload, msg->s_header.s_payload_len);
}

bool create_msg(Message *msg, MessageType type, const char *__format, ...)
{

    va_list args;
    va_start(args, __format);

    msg->s_header.s_magic = MESSAGE_MAGIC;
    msg->s_header.s_type = type;

    const int length = vsnprintf(msg->s_payload, MAX_PAYLOAD_LEN, __format, args);

    if (length < 0)
    {
        return false;
    }

    msg->s_header.s_payload_len = length;

    return true;
}

bool create_msg_empty(Message *msg, MessageType type)
{
    msg->s_header.s_magic = MESSAGE_MAGIC;
    msg->s_header.s_type = type;
    msg->s_header.s_payload_len = 0;

    return true;
}

// warning! uses blocking IO
int receive_all_X_msg(proc_data proc_data, int16_t type)
{
    for (local_id id = 1; id < proc_data.proc_amount; ++id)
    {

        if (id == proc_data.local_id)
        {
            continue;
        }

        Message msg_r1;
        if (receive_block(&proc_data, id, &msg_r1) < 0)
        {
            perror("Error in msg_r1");
            return -1;
        }
        assert(msg_r1.s_header.s_type == type);
    }

    return 0;
}

int receive_all_started_msg(proc_data proc_data)
{
    return receive_all_X_msg(proc_data, STARTED);
}

int receive_all_done_msg(proc_data proc_data)
{
    return receive_all_X_msg(proc_data, DONE);
}

void sync_history(BalanceHistory *balanceHistory, proc_data proc_data, timestamp_t timestamp)
{
    for (timestamp_t current_history_len = balanceHistory->s_history_len; current_history_len < timestamp; ++current_history_len)
    {
        balanceHistory->s_history[current_history_len].s_time = current_history_len;
        balanceHistory->s_history[current_history_len].s_balance = proc_data.balance;
        balanceHistory->s_history[current_history_len].s_balance_pending_in = 0; // must be 0 on pa2
    }

    balanceHistory->s_history_len = timestamp;
}

void send_history(BalanceHistory *history, proc_data proc_data)
{
    Message msg;
    msg.s_header.s_magic = MESSAGE_MAGIC;
    msg.s_header.s_type = BALANCE_HISTORY;
    // TODO: check offset
    msg.s_header.s_payload_len = offsetof(BalanceHistory, s_history) + sizeof(BalanceState) * history->s_history_len;
    memcpy(msg.s_payload, history, msg.s_header.s_payload_len);

    send(&proc_data, PARENT_ID, &msg);
}

int child_job(proc_data proc_data)
{
    // started
    Message msg_started;
    create_msg(&msg_started, STARTED, log_started_fmt, get_lamport_time(), proc_data.local_id, getpid(), proc_data.parent_pid, proc_data.balance);

    flogger(proc_data.event_fd, log_started_fmt, get_lamport_time(), proc_data.local_id, getpid(), proc_data.parent_pid, proc_data.balance);
    send_multicast(&proc_data, &msg_started);
    receive_all_started_msg(proc_data);
    flogger(proc_data.event_fd, log_received_all_started_fmt, get_lamport_time(), proc_data.local_id);

    // banking

    // init history
    BalanceHistory balanceHistory = {.s_id = proc_data.local_id, .s_history_len = 0};
    int stop = 0;
    while (stop == 0)
    {

        // banking loop
        Message msg;
        local_id src_id;

        src_id = receive_any_with_id(&proc_data, &msg);

        const timestamp_t timestamp = get_lamport_time(); // implemented in runtime.so
        sync_history(&balanceHistory, proc_data, timestamp);

        switch (msg.s_header.s_type)
        {
        case STARTED:
        {
            proc_data.context.started++;
            break;
        }

        case DONE:
        {
            proc_data.context.done++;
            break;
        }

        case CS_REPLY:
        {
            proc_data.context.replies++;
            break;
        }

        case TRANSFER:
        {

            TransferOrder *transfer = (TransferOrder *)msg.s_payload;
            if (transfer->s_src == proc_data.local_id)
            {
                // sending money to another node
                proc_data.balance = proc_data.balance - transfer->s_amount;
                flogger(proc_data.event_fd, log_transfer_out_fmt, timestamp, proc_data.local_id, transfer->s_amount, transfer->s_dst);

                send(&proc_data, transfer->s_dst, &msg); // just re-send message from parent to dst node
            }
            else
            {
                // receiving money from another node
                proc_data.balance = proc_data.balance + transfer->s_amount;

                for (timestamp_t i = msg.s_header.s_local_time - 1; i < timestamp; ++i)
                {
                    balanceHistory.s_history[i].s_balance_pending_in += transfer->s_amount;
                }

                flogger(proc_data.event_fd, log_transfer_in_fmt, timestamp, proc_data.local_id, transfer->s_amount, transfer->s_src);

                Message ack_msg;
                create_msg_empty(&ack_msg, ACK);

                send(&proc_data, PARENT_ID, &ack_msg); // inform parent
            }

            break;
        }

        case STOP:
        {
            // just save last balance before end

            int curr_len = balanceHistory.s_history_len;

            balanceHistory.s_history[curr_len].s_time = balanceHistory.s_history_len;
            balanceHistory.s_history[curr_len].s_balance = proc_data.balance;
            balanceHistory.s_history[curr_len].s_balance_pending_in = 0;
            balanceHistory.s_history_len++;

            stop = 1;
            break;
        }

        case CS_REQUEST:
        {
            cs_queue_add_index(proc_data.cs_queue, src_id, msg.s_header.s_local_time);

            Message reply_msg;
            create_msg_empty(&reply_msg, CS_REPLY);
            send(&proc_data, src_id, &reply_msg);

            break;
        }

        case CS_RELEASE:
        {
            // TODO: maybe add some assertion?
            cs_queue_pop(proc_data.cs_queue);
        }
        }
    }

    // done

    Message msg_done;
    create_msg(&msg_done, DONE, log_done_fmt, get_lamport_time(), proc_data.local_id, proc_data.balance);

    flogger(proc_data.event_fd, log_done_fmt, get_lamport_time(), proc_data.local_id, proc_data.balance);
    send_multicast(&proc_data, &msg_done);

    // receive_all_done_msg(proc_data);
    flogger(proc_data.event_fd, log_received_all_done_fmt, get_lamport_time(), proc_data.local_id);

    send_history(&balanceHistory, proc_data);

    return 0;
}

void close_pipes(pipe_io *pipes, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (pipes[i].in != -1)
        {
            close(pipes[i].in);
        }
        if (pipes[i].out != -1)
        {
            close(pipes[i].out);
        }

        FILE *pipe_log = open_pipe_logfile();
        // flogger(pipe_log, PIPE_CLOSED_F, pipes[i].in, pipes[i].out);
        close_file(pipe_log);
    }
}

void convert_pipes(int local_id, pipe_io *pipes, int proc_amount, pipe_io *pipes_p)
{
    int amount = proc_amount * (proc_amount - 1);
    for (int i = 0; i < proc_amount; i++)
    {
        if (i == local_id)
        {
            continue;
        }
        int r_index = pipe_pos_count_reader(local_id, i, proc_amount);
        pipes_p[r_index].in = pipes[r_index].in;
        pipes_p[r_index].out = -1; //pipes[r_index].out;

        pipes[r_index].in = -1;

        int w_index = pipe_pos_count_writer(local_id, i, proc_amount);
        pipes_p[w_index].in = -1; //pipes[w_index].in;
        pipes_p[w_index].out = pipes[w_index].out;

        pipes[w_index].out = -1;
    }

    close_pipes(pipes, amount);
}

pid_t create_child_proccess(int local_id,
                            FILE *log_file,
                            pipe_io *pipes,
                            int proc_amount,
                            int use_mutex)
{
    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid == 0)
    {

        int amount = proc_amount * (proc_amount - 1);

        pipe_io *pipes_p = malloc(sizeof(pipe_io) * amount);
        convert_pipes(local_id, pipes, proc_amount, pipes_p);
        free(pipes);

        proc_data proc_data = {
            .local_id = local_id,
            .pipes = pipes_p,
            .proc_amount = proc_amount,
            .parent_pid = parent_pid,
            .event_fd = log_file,

            .use_mutex = use_mutex,
            .cs_queue = {.amount = 0},
            .context = {0}};

        // child
        int job_s = child_job(proc_data);
        exit(job_s);
    }
    else
    {

        // parent
    }

    return pid;
}

int kill(pid_t pid, int sig);

int send_stop_msg_to_all(proc_data proc_data)
{

    Message msg;

    msg.s_header.s_magic = MESSAGE_MAGIC;
    msg.s_header.s_type = STOP;
    msg.s_header.s_payload_len = 0;

    if (send_multicast(&proc_data, &msg) < 0)
    {
        return -1;
    }
    else
    {
        return 0;
    }
}

int receive_history(AllHistory *historyAll, proc_data proc_data)
{
    historyAll->s_history_len = proc_data.proc_amount - 1;

    for (local_id id = 1; id < proc_data.proc_amount; ++id)
    {
        Message msg;

        if (receive_block(&proc_data, id, &msg) < 0)
        {
            return -1;
        }

        assert(msg.s_header.s_type == BALANCE_HISTORY);

        const BalanceHistory *const history = (BalanceHistory *)msg.s_payload;

        historyAll->s_history[id - 1].s_id = history->s_id;
        historyAll->s_history[id - 1].s_history_len = history->s_history_len;

        for (size_t i = 0; i < history->s_history_len; ++i)
        {
            historyAll->s_history[id - 1].s_history[i] = history->s_history[i];
        }
    }

    return 0;
}

int parent_wait(proc_data proc_data)
{

    flogger(proc_data.event_fd, log_started_fmt, get_lamport_time(), proc_data.local_id, getpid(), proc_data.parent_pid, proc_data.balance);
    receive_all_started_msg(proc_data);
    flogger(proc_data.event_fd, log_received_all_started_fmt, get_lamport_time(), proc_data.local_id);

    bank_robbery(&proc_data, proc_data.proc_amount - 1);

    send_stop_msg_to_all(proc_data);

    flogger(proc_data.event_fd, log_done_fmt, get_lamport_time(), proc_data.local_id, proc_data.balance);
    receive_all_done_msg(proc_data);
    flogger(proc_data.event_fd, log_received_all_done_fmt, get_lamport_time(), proc_data.local_id);

    AllHistory allHistory;
    receive_history(&allHistory, proc_data);

    print_history(&allHistory);

    return 0;
}

void start(int proc_amount, int use_mutex)
{

    fclose(fopen(events_log, "w"));
    fclose(fopen(pipes_log, "w"));

    FILE *event_file = open_event_logfile();
    pid_t pids[proc_amount - 1];

    int amount = proc_amount * (proc_amount - 1);
    pipe_io *pipes = malloc(sizeof(pipe_io) * amount);

    create_pipes(proc_amount * (proc_amount - 1), pipes);

    for (int i = 1; i < proc_amount; i++)
    {
        pids[i - 1] = create_child_proccess(i, event_file, pipes, proc_amount, use_mutex);
    }

    pipe_io *pipes_p = malloc(sizeof(pipe_io) * amount);
    convert_pipes(PARENT_ID, pipes, proc_amount, pipes_p);
    close_pipes(pipes, amount);

    free(pipes);

    proc_data parent_proc_data = {
        .local_id = PARENT_ID,
        .event_fd = event_file,
        .parent_pid = getpid(),
        .pipes = pipes_p,
        .proc_amount = proc_amount,

        .use_mutex = use_mutex,
        .cs_queue = {.amount = 0},
        .context = {0}};

    parent_wait(parent_proc_data);

    fclose(event_file);
    free(pipes_p);

    int status;
    pid_t wpid;
    while ((wpid = wait(&status)) > 0)
        ;
    for (int i = 0; i < proc_amount - 1; i++)
    {
        kill(pids[i], SIGKILL);
    }
}

/** Send a message to the process specified by id.
 *
 * @param self    Any data structure implemented by students to perform I/O
 * @param dst     ID of recepient
 * @param msg     Message to send
 *
 * @return 0 on success, any non-zero value on error
 */
int send(void *self, local_id dst, const Message *msg)
{
    const proc_data *proc_data = self;
    const int pipe_index = pipe_pos_count_writer(proc_data->local_id, dst, proc_data->proc_amount);

    Message lamport_msg; // we need to create new message, because msg is const
    set_lamport_time(msg, &lamport_msg);

    write(proc_data->pipes[pipe_index].out, &lamport_msg, sizeof(MessageHeader) + lamport_msg.s_header.s_payload_len);

    return 0;
}

int send_without_prep(void *self, local_id dst, const Message *msg)
{
    const proc_data *proc_data = self;
    const int pipe_index = pipe_pos_count_writer(proc_data->local_id, dst, proc_data->proc_amount);

    write(proc_data->pipes[pipe_index].out, msg, sizeof(MessageHeader) + msg->s_header.s_payload_len);

    return 0;
}

/** Send multicast message.
 *
 * Send msg to all other processes including parrent.
 * Should stop on the first error.
 * 
 * @param self    Any data structure implemented by students to perform I/O
 * @param msg     Message to multicast.
 *
 * @return 0 on success, any non-zero value on error
 */
int send_multicast(void *self, const Message *msg)
{
    const proc_data *proc_data = self;
    Message lamport_msg; // we need to create new message, because msg is const
    set_lamport_time(msg, &lamport_msg);

    for (local_id id = 0; id < proc_data->proc_amount; ++id)
    {

        if (id == proc_data->local_id)
        {
            continue;
        }

        int send_res = send_without_prep(self, id, &lamport_msg);

        if (send_res != 0)
        {
            return -1;
        }
    }

    return 0;
}

/** Receive a message from the process specified by id.
 *
 * Might block depending on IPC settings.
 *
 * @param self    Any data structure implemented by students to perform I/O
 * @param from    ID of the process to receive message from
 * @param msg     Message structure allocated by the caller
 *
 * @return 0 on success, any non-zero value on error
 */
int receive(void *self, local_id from, Message *msg)
{
    const proc_data *proc_data = self;
    const int pipe_index = pipe_pos_count_reader(proc_data->local_id, from, proc_data->proc_amount);

    if (0 > read(proc_data->pipes[pipe_index].in, &(msg->s_header), sizeof(MessageHeader)))
    {
        return -1;
    }

    if (0 > read(proc_data->pipes[pipe_index].in, msg->s_payload, msg->s_header.s_payload_len))
    {

        return -20;
    }

    if (lamport_time < msg->s_header.s_local_time)
    {
        lamport_time = msg->s_header.s_local_time;
    }

    lamport_time++;

    return 0;
}

/** Receive a message from any process.
 *
 * Receive a message from any process, in case of blocking I/O should be used
 * with extra care to avoid deadlocks.
 *
 * @param self    Any data structure implemented by students to perform I/O
 * @param msg     Message structure allocated by the caller
 *
 * @return 0 on success, any non-zero value on error
 */
int receive_any(void *self, Message *msg)
{
    const proc_data *proc_data = self;
    while (true)
    {
        for (local_id id = 0; id < proc_data->proc_amount; ++id)
        {
            if (id == proc_data->local_id)
            {
                continue;
            }

            if (receive(self, id, msg) == 0)
            {
                return 0;
            }
        }
    }

    return 0;
}

int receive_any_with_id(void *self, Message *msg)
{
    const proc_data *proc_data = self;
    while (true)
    {
        for (local_id id = 0; id < proc_data->proc_amount; ++id)
        {
            if (id == proc_data->local_id)
            {
                continue;
            }

            if (receive(self, id, msg) == 0)
            {
                return id;
            }
        }
    }

    return -1;
}

void transfer(void *parent_data, local_id src, local_id dst,
              balance_t amount)
{

    proc_data *proc_data = parent_data;

    // message for send
    Message msg;
    msg.s_header.s_magic = MESSAGE_MAGIC;
    msg.s_header.s_type = TRANSFER;
    msg.s_header.s_payload_len = sizeof(TransferOrder);

    // creating transfer message
    TransferOrder *const transfer_order = (TransferOrder *)msg.s_payload;
    transfer_order->s_src = src;
    transfer_order->s_dst = dst;
    transfer_order->s_amount = amount;

    send(parent_data, src, &msg);

    Message msg_r;
    receive_block(proc_data, dst, &msg_r);
    assert(msg_r.s_header.s_type = ACK);
}

int main(int argc, char *argv[])
{
    //bank_robbery(parent_data);
    //print_history(all);

    int proc_amount;
    int use_mutex = 0;
    if (-1 == parse_arg(argc, argv, &proc_amount, &use_mutex))
    {
        puts("Invalid format. Use -p ");
        return -1;
    }

    if (proc_amount < 0 || proc_amount > MAX_PROCESS_ID + 1)
    {
        puts("Invalid size of processes amount");
        return -2;
    }

    start(proc_amount, use_mutex);

    // printf("%d %d", pipe_pos_count_reader(2, 0, 3), pipe_pos_count_writer(2, 1, 3));

    return 0;
}
