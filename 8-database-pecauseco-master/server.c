#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;  // set this to one, and unlock and lock
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
server_control_t controller1 = {PTHREAD_MUTEX_INITIALIZER,
                                PTHREAD_COND_INITIALIZER, 0};
server_control_t *controller = &controller1;
client_control_t client_controller1 = {PTHREAD_MUTEX_INITIALIZER,
                                       PTHREAD_COND_INITIALIZER, 0};
client_control_t *client_controller = &client_controller1;
int accepting;

void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

/*
 * Checks to see if the client needs to wait by seeing if the controllers stop
 * field has been edited and is called in run client
 *
 * Returns:
 *  - nothing
 */
void client_control_wait() {
    int err;
    pthread_mutex_lock(&client_controller->go_mutex);
    while (client_controller->stopped == 1) {
        if ((err = pthread_cond_wait(&client_controller->go,
                                     &client_controller->go_mutex)) != 0) {
            handle_error_en(err, "pthread function error");
        }
    }
    pthread_mutex_unlock(&client_controller->go_mutex);
}

// Called by main thread to stop client threads
/*
 * This method stops all the client threads and changes the client stopped field
 * so that the wait function stops the clients
 *
 * Returns:
 *  - nothing
 */
void client_control_stop() {
    pthread_mutex_lock(&client_controller->go_mutex);
    client_controller->stopped = 1;
    pthread_mutex_unlock(&client_controller->go_mutex);
}

/*
 * Releases all the clients if they have been stopped and are currently waiting
 * to be started again
 *
 * Returns:
 *  - nothing
 */
void client_control_release() {
    int err;
    pthread_mutex_lock(&client_controller->go_mutex);
    client_controller->stopped = 0;
    if ((err = pthread_cond_broadcast(&client_controller->go)) != 0) {
        handle_error_en(err, "pthread error");
    }
    pthread_mutex_unlock(&client_controller->go_mutex);
}

/*
 * The client constructor that creates a client by setting its initial field
 * values, and mallocs space for it. It also starts the client thread
 *
 * Parameters:
 *  - cxstr: a pointer to the stream
 *
 * Returns:
 *  - nothing
 */
void client_constructor(FILE *cxstr) {
    int err;
    client_t *client = malloc(sizeof(client_t));
    if (client == NULL) {
        perror("malloc");
    }
    client->cxstr = cxstr;
    client->prev = NULL;
    client->next = NULL;
    pthread_t client_thread;

    if ((err = pthread_create(&client_thread, NULL, run_client,
                              (void *)client)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    client->thread = client_thread;
    if ((err = pthread_detach(client->thread)) != 0) {
        handle_error_en(err, "pthread function error");
    }
}

/*
 * Destroys the client created by the client thread
 *
 * Parameters:
 *  - client: the client being destroyed
 *
 * Returns:
 *  - nothing
 */
void client_destructor(client_t *client) {
    comm_shutdown(client->cxstr);

    free((void *)client);
}

void *run_client(void *arg) {
    if (accepting == 1) {
        int connection = 0;
        char response[BUFLEN];
        response[0] = 0;
        char command[BUFLEN];
        if (thread_list_head == NULL) {
            thread_list_head = arg;
            ((client_t *)arg)->prev = arg;
            ((client_t *)arg)->next = arg;
        } else {
            ((client_t *)arg)->prev = thread_list_head->prev;
            ((client_t *)arg)->next = thread_list_head;
            thread_list_head->prev->next = arg;
            thread_list_head->prev = arg;
            thread_list_head = arg;
        }
        pthread_mutex_lock(&controller->server_mutex);
        controller->num_client_threads++;
        pthread_mutex_unlock(&controller->server_mutex);
        pthread_cleanup_push(thread_cleanup, ((client_t *)arg));
        while (connection != -1) {
            connection =
                comm_serve(((client_t *)arg)->cxstr, response, command);
            client_control_wait();
            interpret_command(command, response, BUFLEN);
        }
        pthread_cleanup_pop(1);
    }
    return NULL;
}

/*
 * Deletes all of the threads by cancelling them, and starting their cleanup
 * routine. Called either by SIGINT or EOF
 *
 * Returns:
 *  - nothing
 */
void delete_all() {
    int err;
    if (thread_list_head != NULL) {
        pthread_mutex_lock(&thread_list_mutex);
        client_t *curr = thread_list_head->next;
        if ((err = pthread_cancel(thread_list_head->thread)) != 0) {
            handle_error_en(err, "pthread function error");
        }
        while (curr != thread_list_head) {
            if ((err = pthread_cancel(curr->thread)) != 0) {
                handle_error_en(err, "pthread function error");
            }
            curr = curr->next;
        }

        pthread_mutex_unlock(&thread_list_mutex);
    }
}

/*
 * The thread cleanup method which calls the destructor and removes the client
 * from the client list
 *
 * Parameters:
 *  - arg: a pointer to the client we are removing
 *
 * Returns:
 *  - nothing
 */
void thread_cleanup(void *arg) {
    int err;
    if (thread_list_head == ((client_t *)arg) &&
        ((client_t *)arg)->next == thread_list_head) {
        thread_list_head = NULL;
    } else {
        if (thread_list_head == ((client_t *)arg)) {
            thread_list_head = ((client_t *)arg)->next;
        }
        ((client_t *)arg)->prev->next = ((client_t *)arg)->next;
        ((client_t *)arg)->next->prev = ((client_t *)arg)->prev;
    }
    client_destructor((client_t *)arg);
    pthread_mutex_lock(&controller->server_mutex);
    controller->num_client_threads--;
    if (controller->num_client_threads == 0) {
        if ((err = pthread_cond_signal(&controller->server_cond)) != 0) {
            handle_error_en(err, "pthread function error");
        }
    }
    pthread_mutex_unlock(&controller->server_mutex);
}

/*
 * This method waits for a SIGINT and if SIGINT is given, then it begins the
 * delete_all method to get rid of all of the threads
 *
 * Parameters:
 *  - arg: a pointer to the signal handler's signal set
 *
 * Returns:
 *  - void pointer
 */
void *monitor_signal(void *arg) {
    int err;
    int sig;
    while (1) {
        if ((err = sigwait((sigset_t *)arg, &sig)) != 0) {
            fprintf(stderr, "sigwait error");
        }
        delete_all();
    }

    return NULL;
}

/*
 * The constructor for the signal handler which is scanning for SIGINT so that
 * it deletes all of the threads. All of its fields are initialized here and its
 * thread is started
 *
 * Returns:
 *  - the sighandler that was created
 */
sig_handler_t *sig_handler_constructor() {
    int err;
    sig_handler_t *sig_handler = malloc(sizeof(sig_handler_t));
    if (sig_handler == NULL) {
        perror("malloc");
    }
    sigemptyset(&sig_handler->set);
    sigaddset(&sig_handler->set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sig_handler->set, 0);
    pthread_t sig_thread;

    if ((err = pthread_create(&sig_thread, NULL, monitor_signal,
                              (void *)&sig_handler->set)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    sig_handler->thread = sig_thread;

    return sig_handler;
}

/*
 * This method destroys the sighandler and frees up the strcut that we amlloced
 * in the constructor
 *
 * Parameters:
 *  - sighandler: the sig_handler_t that we want to destroy
 *
 * Returns:
 *  - nothing
 */
void sig_handler_destructor(sig_handler_t *sighandler) {
    int err;
    if ((err = pthread_cancel(sighandler->thread)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    if ((err = pthread_join(sighandler->thread, NULL)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    free(sighandler);
}

/*
 * Parses up input from the command line to be read and intepreted into commands
 * for the server
 *
 * Parameters:
 *  - buffer: an array containing the input the user types in
 *  - tokens: an array that will contain the parsed inputs
 *  - argv: an array containing the parsed inputs without the full filepath
 *
 * Returns:
 *  - nothing
 */
void parse(char buffer[1024], char *tokens[512], char *argv[512]) {
    char *str = &buffer[0];  // sets a pointer to the buffer array
    int i = 0;
    char *tokenArray = strtok(str, " \t\n");
    while (tokenArray != NULL) {
        tokens[i] = tokenArray;
        char *modified = tokenArray;
        char *occurrence = strrchr(modified, '/');
        if (occurrence == NULL && i == 0) {
            argv[0] = tokenArray;  // if Null and first word
        } else if (occurrence != NULL &&
                   i == 0) {  // if slash is in file and it is first token word
            argv[0] = occurrence + 1;  // next word
        } else {
            argv[i] = tokenArray;  // else just make arg the token word
        }
        tokenArray = strtok(NULL, " \t\n");  // move on to next word
        i++;                                 // add one to index
    }
}

/*
 * The main method that calls the constructors for things like the sighandler
 * and also creates the listener thread. This method also contains the REPL
 * which will take in user input and respond accordingly. After the REPL, if EOF
 * is receieved the clean up procedure begins and everything is freed and the
 * database cleans up all the threads.
 *
 * Parameters:
 *  - argc: number of arguments
 *  - argv: argument array
 *
 * Returns:
 *  - nothing
 */
int main(int argc, char *argv[]) {
    int err;
    accepting = 1;

    char buffer[1024];
    char *tokens[512];
    char *argv2[512];

    sig_handler_t *sig_handler = sig_handler_constructor();
    signal(SIGPIPE, SIG_IGN);
    pthread_t listener = start_listener(atoi(argv[1]), client_constructor);  //
    while (1) {
        memset(&argv2[0], 0, 512 * sizeof(char *));
        memset(&buffer[0], 0, 1024 * sizeof(char));
        memset(&tokens[0], 0, 512 * sizeof(char *));
        ssize_t buffer_size = read(0, buffer, 1024);

        parse(buffer, tokens, argv2);
        if (buffer_size == -1) {
            fprintf(stderr, "Reading input failed \n");
        } else if (buffer_size > 1024) {
            fprintf(stderr, "input is too long \n");
        } else if (buffer_size == 0) {
            /* in the case of ctrl-d */
            accepting = 0;
            fprintf(stdout, "exiting database\n");
            break;
        }
        if (tokens[0] == 0) {
            continue;
        }

        if (strcmp(tokens[0], "p") == 0) {
            db_print(tokens[1]);
        } else if (strcmp(tokens[0], "s") == 0) {
            fprintf(stdout, "stopping all clients\n");
            client_control_stop();
        } else if (strcmp(tokens[0], "g") == 0) {
            fprintf(stdout, "releasing all clients\n");
            client_control_release();
        }
    }

    delete_all();
    pthread_mutex_lock(&controller->server_mutex);
    while (controller->num_client_threads != 0) {
        if ((err = pthread_cond_wait(&controller->server_cond,
                                     &controller->server_mutex)) != 0) {
            handle_error_en(err, "pthread function error");
        }
    }
    pthread_mutex_unlock(&controller->server_mutex);
    sig_handler_destructor(sig_handler);
    if ((err = pthread_cond_destroy(&client_controller->go)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    if ((err = pthread_mutex_destroy(&client_controller->go_mutex)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    if ((err = pthread_cond_destroy(&controller->server_cond)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    if ((err = pthread_mutex_destroy(&controller->server_mutex)) != 0) {
        handle_error_en(err, "pthread function error");
    }

    if ((err = pthread_cancel(listener)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    if ((err = pthread_join(listener, NULL)) != 0) {
        handle_error_en(err, "pthread function error");
    }
    db_cleanup();

    return 0;
}