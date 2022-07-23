#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#include "libnnfs_socket.h"
#include "libnnfs_msg_builder.h"
#include "nnfs_constants.h"
#include "libnnfs_primitive_matcher.h"
#include "libnnfs_queue.h"

const char* server_menu = "MENU:write one whole command, p.e. bind 0.0.0.0:24004\n1)bind ip_address:port\n2)start max_clients\n\n";

#define COMMAND_MAX_LENGTH 64u
#define MAX_NUMBER_OF_CLIENTS 16u



//TODO: rework the client and the server
//1)add a quit sequence for both sides
//3)if possible move logging to a different terminal

void server_start(struct nnfs_context *server, uint16_t client_number);

int main(){
    uint32_t ID = STARTING_ID;
    struct nnfs_context server;
    nnfs_init_context(&server);

    int result = 0;
    char *command = calloc(1, COMMAND_MAX_LENGTH + 1);
    if(command == NULL){
        printf("ERROR: calloc in main server returned NULL\n");
        exit(1);
    }
    bool infinite_loop = true;
    uint32_t command_op_code = NULL_OP_CODE;
    char *ip = NULL, *port = NULL;

    while(infinite_loop){
        printf("%s", server_menu);
        fgets(command, COMMAND_MAX_LENGTH, stdin);
        command_op_code = type_of_command(command);
        switch(command_op_code){
            case SERVER_BIND:
                match_IPaddr(command, &ip);
                match_PORTnumber(command, &port);
                if(ip != NULL && port!=NULL){
                    printf("SUCCESS: binding to %s at %s\n", port, ip);
                    result = nnfs_bind(&server, ip, port);
                } else{
                    result = 1;
                    if(port == NULL)
                        printf("ERROR: port should be separated by \":\" from IP: <ip>:<port>\n");
                    if(ip == NULL)
                        printf("ERROR: ip numbers should be divided by \".\": <number>.<number>.<number>.<number>\n");
                }
                free(ip);
                ip = NULL;
                free(port);
                port = NULL;
                if(result != 0){
                    printf("ERROR: error binding. try changing port\n");
                }
                else
                    printf("SUCCESS: successful binding\n");
                break;
            
            case SERVER_LISTEN_AND_ACCEPT:
                result = match_client_number(command);
                if(result > 0 && result <= MAX_NUMBER_OF_CLIENTS){
                    server_start(&server, result);
                }
                else{
                    printf("ERROR: invalid arguments -number of clients should be between 1 and %d\n", MAX_NUMBER_OF_CLIENTS);
                }
                break;
            
            default:
                printf("ERROR: invalid user command\n");
                break;
        }    
    }
}

//my first attempt in threads will look wonky
//will put it into a different file later

//counting threads for debugging
atomic_char16_t counter = 0;


sem_t job_status;
pthread_mutex_t queue_mutex;
struct queue_node *head;
//only one thread changes this variable and once, therefore no protection is needed
bool shutting_down = false;


void workload(struct nnfs_context *client, int thread_number){
    bool infinite_loop = true;
    struct MSG message;
    init_msg(&message);

    while(infinite_loop){
        nnfs_receive(client, &message);
        switch(message.header.op_code){
            case OP_CODE_PING:
                printf("RESPONSE: thread %d is doing ping-pong message\n",thread_number);
                build_pong_reply(&message);
                nnfs_send(client, &message);
                break;
            case OP_CODE_CLOSE_CONNECTION:
                printf("RESPONSE: thread number %d is shutting down connection with the client: Client logged off\n", thread_number);
                build_success_reply(&message, message.header.ID);
                nnfs_send(client, &message);
                nnfs_shutdown(client);
                printf("SUCCESS: quit message has been sent\n");
                infinite_loop = false;
                break;
            default:
                build_fail_reply(&message, message.header.ID, STATUS_FAIL_BAD_OP_CODE, 0, NULL);
                nnfs_send(client, &message);
                break;
        }
    }
}

void* acquiring_job(){
    //searching for a client
    //TODO:
    //pthread_mutex_lock(&counting_mutex);
    uint16_t thread_number = counter++;
    //pthread_mutex_unlock(&counting_mutex);

    struct nnfs_context client;
    while(!shutting_down){
        bool has_job = false;
        while(!has_job){
            sem_wait(&job_status);
            pthread_mutex_lock(&queue_mutex);
            printf("STATUS: Thread %d got a job\n", thread_number);               
            queue_pop(&head, &client);
            pthread_mutex_unlock(&queue_mutex);
            has_job = true;
        }
        workload(&client, thread_number);
    }
}

//right now there are no race conditions because every thread operates on its own client, so they have their own non-intersecting contexts
//every variable is different for every thread
//will be changed later after adding functionality that requires mutex
void server_start(struct nnfs_context *server, uint16_t client_number){

    bool working = true;
    uint32_t number_of_threads = client_number;
    pthread_t *threads = calloc(sizeof(pthread_t), client_number);
    if(threads == NULL){
        printf("ERROR: calloc in server_start returned NULL\n");
    }

    counter = 0u;
    init_queue(&head);
    struct nnfs_context client_buffer;
    sem_init(&job_status, 0, 0);
    pthread_mutex_init(&queue_mutex, NULL);
    shutting_down = false;

    printf("STATUS: starting up...\n");
    for(int i = 0; i < client_number; i++){
        printf("STATUS: creating thread %d\n", i);
        if(pthread_create(threads + i, NULL,(void* (*)(void*)) &acquiring_job, NULL) != 0){
            printf("ERROR: couldnt create thread\n");
            exit(3);
        }
        printf("STATUS: created successfully\n");
    }
    nnfs_listen(server, client_number);
    printf("STATUS: started listening\n\n");
    //no need to join them because it will run until its stopped from outside
    while(!shutting_down){
        nnfs_accept(server, &client_buffer);
        printf("STATUS: client accepted\n");
        pthread_mutex_lock(&queue_mutex);
        queue_push(&head, &client_buffer);
        pthread_mutex_unlock(&queue_mutex);
        printf("STATUS: added client to the job pool. waiting to be picked up\n");
        sem_post(&job_status);
    }
    //unreachable code right now
    sem_destroy(&job_status);
    pthread_mutex_destroy(&queue_mutex);
    nnfs_close(server);
}