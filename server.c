#include "server.h"

#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <dirent.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h> 
#include <unistd.h>
#include <pthread.h>

#include "common.h"
#include "logger.h"
#include "task.h"
#include "sha1.h"

//added LOG file variable
#define LOG_FILE "task_log.txt"

//added FILE pointer to log file
FILE *log_file;

static char current_block[MAX_BLOCK_LEN];
static uint32_t current_difficulty_mask = 0x0000FFFF;


pthread_mutex_t lock;

struct options {
    int random_seed;
    char* adj_file;
    char* animal_file;
    char* log_file;
};

struct options default_options = {0, "adjectives", "animals", "task_log.txt"};


union msg_wrapper current_task(void)
{
    union msg_wrapper wrapper = create_msg(MSG_TASK);
    struct msg_task *task = &wrapper.task;
    strcpy(task->block, current_block);
    task->difficulty = current_difficulty_mask;
    return wrapper;
}

void print_usage(char *prog_name)
{
    printf("Usage: %s port [-s seed] [-a adjective_file] [-n animal_file] [-l log_file]" , prog_name);
    printf("\n");
    printf("Options:\n"
"    * -s    Specify the seed number\n"
"    * -a    Specify the adjective file to be used\n"
"    * -n    Specify the animal file to be used\n"
"    * -l    Specify the log file to be used\n");
    printf("\n");
}

void handle_heartbeat(int fd, struct msg_heartbeat *hb)
{
    LOG("[HEARTBEAT] %s\n", hb->username);
    union msg_wrapper wrapper = current_task();
    write_msg(fd, &wrapper);
}

void handle_request_task(int fd, struct msg_request_task *req)
{
    LOG("[TASK REQUEST] User: %s, block: %s, difficulty: %u\n", req->username, current_block, current_difficulty_mask);
    union msg_wrapper wrapper = current_task();
    write_msg(fd, &wrapper);
}

void open_log_file(char* file_name) {
    //Try to open the log file (create it if it doesn't already exist)
    log_file = fopen(file_name, "a+");
    if (log_file == NULL) {
        fprintf(stderr, "Error opening task log file\n");
    }
}

void log_task(struct msg_solution *solution) {
    fprintf(
    log_file, 
    "%s\t%d\t%lu\t%s\t%ld\n", 
            solution->block, 
            solution->difficulty, 
            solution->nonce, 
            solution->username, 
            time(NULL));

    //we fflush the file to ensure the write is on the disk after each update
    fflush(log_file);
}

bool verify_solution(struct msg_solution *solution)
{
    uint8_t digest[SHA1_HASH_SIZE];
    const char *check_format = "%s%lu";
    ssize_t buf_sz = snprintf(NULL, 0, check_format, current_block, solution->nonce);
    char *buf = malloc(buf_sz + 1);
    if(buf == NULL){
        perror("malloc");
        return false;
    }

    snprintf(buf, buf_sz + 1, check_format, current_block, solution->nonce);
    sha1sum(digest, (uint8_t *) buf, buf_sz);
    char hash_string[41];
    sha1tostring(hash_string, digest);
    LOG("SHA1sum: '%s' => '%s'\n", buf, hash_string);
    free(buf);

    /* Get the first 32 bits of the hash */
    uint32_t hash_front = 0;
    hash_front |= digest[0] << 24;
    hash_front |= digest[1] << 16;
    hash_front |= digest[2] << 8;
    hash_front |= digest[3];

    /* Check to see if we've found a solution to our block and add it to the log file */
    if ((hash_front & current_difficulty_mask) == hash_front) {
        log_task(solution);
    }
    
    return (hash_front & current_difficulty_mask) == hash_front;
}

void handle_solution(int fd, struct msg_solution *solution)
{
    LOG("[SOLUTION SUBMITTED] User: %s, block: %s, difficulty: %u, NONCE: %lu\n", solution->username, solution->block, solution->difficulty, solution->nonce);
    
    union msg_wrapper wrapper = create_msg(MSG_VERIFICATION);
    struct msg_verification *verification = &wrapper.verification;
    verification->ok = false; // assume the solution is not valid by default

    /* We could directly verify the solution, but let's make sure it's the same
     * block and difficulty first: */
    if (strcmp(current_block, solution->block) != 0)
    {
        strcpy(verification->error_description, "Block does not match current block on server");
        
        //error occurs during the write, return -1
        if(write_msg(fd, &wrapper) == -1) {
            perror("socket write");
        }
        return;
    }
    
    if (current_difficulty_mask !=  solution->difficulty) {
        strcpy(verification->error_description, "Difficulty does not match current difficulty on server");
        write_msg(fd, &wrapper);
        //error occurs during the write, return -1
        if(write_msg(fd, &wrapper) == -1) {
            perror("socket write");
        }
        return;
    }
    
    pthread_mutex_lock(&lock); // lock before verification so that it is only executed by one thread at a time
    verification->ok = verify_solution(solution);

    if (verification->ok) {
        task_generate(current_block); // generate new task if necessary
        LOG("Generated new block: %s\n", current_block);
    }

    pthread_mutex_unlock(&lock); // unlock after verification

    strcpy(verification->error_description, "Verified SHA-1 hash");
    write_msg(fd, &wrapper);
    LOG("[SOLUTION %s!]\n", verification->ok ? "ACCEPTED" : "REJECTED");
    
}

void *client_thread(void* client_fd) {
    int fd = (int) (long) client_fd;
    while (true) {

      union msg_wrapper msg;
       ssize_t bytes_read = read_msg(fd, &msg);
       if(bytes_read == -1){
            perror("read_msg");
            return NULL;
       }
       else if (bytes_read == 0) {
           LOGP("Disconnecting client\n");
            return NULL;
       }

        switch (msg.header.msg_type) {
            case MSG_REQUEST_TASK: handle_request_task(fd, (struct msg_request_task *) &msg.request_task);
                                   break;
            case MSG_SOLUTION: handle_solution(fd, (struct msg_solution *) &msg.solution);
                               break;
            case MSG_HEARTBEAT: handle_heartbeat(fd, (struct msg_heartbeat *) &msg.heartbeat);
                                break;
            default:
                LOG("ERROR: unknown message type: %d\n", msg.header.msg_type);
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char *argv[]) {

    if (pthread_mutex_init(&lock, NULL) != 0) {
        printf("\n mutex init failed\n");
        return 1;
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    struct options opts;
    opts = default_options;

    int port = atoi(argv[1]);
    int c;
    opterr = 0;
    while ((c = getopt(argc, argv, "s:a:n:l:")) != -1) {
        switch (c) {
        char *end;
            case 's':
                opts.random_seed = (int) strtol(optarg, &end, 10);
                LOG("seed is %d\n", opts.random_seed);
                if (end == optarg) {
                    return 1;
                }
                break;
            case 'a':
                opts.adj_file = optarg;
                LOG("adj file is %s\n", opts.adj_file);
                break;
            case 'n':
                opts.animal_file = optarg;
                LOG("animal file is %s\n", opts.animal_file);
                break;
            case 'l':
                opts.log_file = optarg;
                LOG("log file is %s\n", opts.log_file);
                break;
        }
    }
    
    LOG("Starting coin-server version %.1f...\n", VERSION);
    LOG("%s", "(c) 2023 CS 521 Students\n");

    //open the log_file when the server starts up
    open_log_file(opts.log_file);
    
    task_init(opts.random_seed, opts.adj_file, opts.animal_file);
    task_generate(current_block);
    LOG("Current block: %s\n", current_block);

    // create a socket
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        return 1;
    }

    // bind to the port specified above
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(socket_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }

    // start listening for clients to connect
    if (listen(socket_fd, 10) == -1) {
        perror("listen");
        return 1;
    }

    LOG("Listening on port %d\n", port);

    while (true) {
        /* Outer loop: this keeps accepting connection */
        struct sockaddr_in client_addr = { 0 };
        socklen_t slen = sizeof(client_addr);

	// accept client connection
        int client_fd = accept(
                socket_fd,
                (struct sockaddr *) &client_addr,
                &slen);

        if (client_fd == -1) {
            perror("accept");
            return 1;
        }

	// find out their info (host name, port)
        char remote_host[INET_ADDRSTRLEN];
        inet_ntop(
                client_addr.sin_family,
                (void *) &((&client_addr)->sin_addr),
                remote_host,
                sizeof(remote_host));
        LOG("Accepted connection from %s:%d\n", remote_host, client_addr.sin_port);

        pthread_t thread;
        pthread_create(&thread, NULL, client_thread, (void *) (long) client_fd);
        pthread_detach(thread);
    }
    //Closing log_file before we exit the server.
    fclose(log_file);
    pthread_mutex_destroy(&lock);
    return 0; 
}
